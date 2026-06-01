#include "ota_client.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include "display.h"

static const char *TAG = "ota_client";

#define OTA_MANIFEST_BUF_SIZE        1024
#define OTA_MANIFEST_TIMEOUT_MS      6000
#define OTA_DOWNLOAD_TIMEOUT_MS      60000

/* RTC_DATA_ATTR is zero-initialized on cold boot / power loss and preserved
 * across deep sleep + esp_restart() — exactly what we need so a failed OTA
 * stays throttled across wake cycles but a fresh USB-flashed device starts
 * with a clean counter. RTC_NOINIT_ATTR would *not* zero on cold boot and
 * the counter would come up with random RTC memory contents (observed:
 * 0xF88E1B21 ≈ 4.17 billion cycles of throttling after a USB flash). */
static RTC_DATA_ATTR uint32_t s_ota_skip_cycles;

typedef struct {
    char *buf;
    int len;
    bool truncated;
} ota_http_ctx_t;

static esp_err_t manifest_http_event_handler(esp_http_client_event_t *evt)
{
    ota_http_ctx_t *ctx = (ota_http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int remaining = OTA_MANIFEST_BUF_SIZE - ctx->len - 1;
        if (remaining <= 0) {
            ctx->truncated = true;
            return ESP_OK;
        }
        int copy = evt->data_len < remaining ? evt->data_len : remaining;
        if (copy < evt->data_len) ctx->truncated = true;
        memcpy(ctx->buf + ctx->len, evt->data, copy);
        ctx->len += copy;
    }
    return ESP_OK;
}

/* Build "<base_url>/ota/manifest" into the caller's buffer. Handles trailing
 * slash on the base URL the same way api_client.c does for /dashboard. */
static void build_manifest_url(const char *base_url, char *out, size_t out_sz)
{
    size_t base_len = strlen(base_url);
    bool has_slash = base_len > 0 && base_url[base_len - 1] == '/';
    snprintf(out, out_sz, "%s%sota/manifest", base_url, has_slash ? "" : "/");
}

/* Strip a leading 'v' for comparison; matches IDF's default PROJECT_VER
 * (`git describe`) which produces "v0.2.0[-3-gabc[-dirty]]" against a
 * "vX.Y.Z" tag, and matches the API's "vX.Y.Z" latestVersion string. */
static const char *strip_v(const char *s)
{
    if (s && s[0] == 'v') return s + 1;
    return s ? s : "";
}

/*
 * Version comparison: strict equality on the dot-tuple. We don't try to
 * detect "newer", just "different" — anything non-equal triggers an OTA.
 * That keeps downgrades possible (a release can be retracted by republishing
 * an older tag) and avoids the strcmp-lexicographic 9→10 boundary trap (R14
 * in docs/decisions/0005-ota-updates.md). Local dev builds report e.g.
 * "0.1.0-3-gabc1234-dirty"; against a CI tag "0.2.0" those compare unequal
 * and OTA proceeds, which is the desired dev behaviour.
 */
static bool versions_match(const char *running, const char *latest)
{
    return strcmp(strip_v(running), strip_v(latest)) == 0;
}

static bool running_image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

static esp_err_t copy_manifest_string(const char *field,
                                      const char *value,
                                      char *out,
                                      size_t out_sz)
{
    if (out_sz == 0) return ESP_ERR_INVALID_SIZE;

    size_t value_len = strlen(value);
    if (value_len >= out_sz) {
        ESP_LOGW(TAG, "Manifest field %s too long (%u >= %u)",
                 field, (unsigned)value_len, (unsigned)out_sz);
        out[0] = '\0';
        return ESP_FAIL;
    }

    memcpy(out, value, value_len + 1);
    return ESP_OK;
}

static bool host_equals_ci(const char *host, size_t host_len,
                           const char *expected)
{
    size_t expected_len = strlen(expected);
    return host_len == expected_len &&
           strncasecmp(host, expected, expected_len) == 0;
}

static bool download_url_host_allowed(const char *url)
{
    static const char *HTTPS = "https://";
    size_t https_len = strlen(HTTPS);
    if (!url || strncasecmp(url, HTTPS, https_len) != 0) return false;

    const char *authority = url + https_len;
    const char *path = strchr(authority, '/');
    if (!path || path == authority) return false;
    if (memchr(authority, '@', (size_t)(path - authority))) return false;

    /* GitHub Release URLs use default-port HTTPS. Reject explicit ports so
     * the host allow-list stays an exact string comparison. */
    size_t host_len = (size_t)(path - authority);
    return host_equals_ci(authority, host_len, "github.com") ||
           host_equals_ci(authority, host_len, "objects.githubusercontent.com");
}

static esp_err_t fetch_manifest(const char *base_url,
                                const char *token,
                                char *out_version,
                                size_t out_version_sz,
                                char *out_download_url,
                                size_t out_download_url_sz,
                                bool *out_ota_enabled)
{
    *out_ota_enabled = false;
    out_version[0] = '\0';
    out_download_url[0] = '\0';

    char url[256];
    build_manifest_url(base_url, url, sizeof(url));
    ESP_LOGI(TAG, "Fetching OTA manifest: %s", url);

    char *buf = calloc(1, OTA_MANIFEST_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;
    ota_http_ctx_t ctx = { .buf = buf, .len = 0, .truncated = false };

    esp_http_client_config_t hcfg = {
        .url = url,
        .event_handler = manifest_http_event_handler,
        .user_data = &ctx,
        .timeout_ms = OTA_MANIFEST_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        free(buf);
        return ESP_FAIL;
    }

    if (token && token[0] != '\0') {
        char auth[96];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Manifest fetch failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        free(buf);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    if (ctx.truncated) {
        ESP_LOGW(TAG, "Manifest body truncated at %d bytes", ctx.len);
        free(buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Manifest JSON parse failed");
        return ESP_FAIL;
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "otaEnabled");
    if (enabled && cJSON_IsTrue(enabled)) {
        *out_ota_enabled = true;
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "latestVersion");
        const cJSON *dl  = cJSON_GetObjectItemCaseSensitive(root, "downloadUrl");
        if (ver && cJSON_IsString(ver)) {
            esp_err_t copy_err = copy_manifest_string("latestVersion",
                                                      ver->valuestring,
                                                      out_version,
                                                      out_version_sz);
            if (copy_err != ESP_OK) {
                cJSON_Delete(root);
                return copy_err;
            }
        }
        if (dl && cJSON_IsString(dl)) {
            esp_err_t copy_err = copy_manifest_string("downloadUrl",
                                                      dl->valuestring,
                                                      out_download_url,
                                                      out_download_url_sz);
            if (copy_err != ESP_OK) {
                cJSON_Delete(root);
                return copy_err;
            }
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static void describe_next_slot(char *name, size_t name_sz,
                               char *label, size_t label_sz)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        strlcpy(name, "ota", name_sz);
        strlcpy(label, "?", label_sz);
        return;
    }

    if (next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        strlcpy(name, "app0", name_sz);
        strlcpy(label, "A", label_sz);
    } else if (next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        strlcpy(name, "app1", name_sz);
        strlcpy(label, "B", label_sz);
    } else {
        strlcpy(name, "ota", name_sz);
        strlcpy(label, "?", label_sz);
    }
}

static esp_err_t download_and_install(const char *download_url,
                                      const char *latest_version)
{
    if (!download_url_host_allowed(download_url)) {
        ESP_LOGW(TAG, "Rejecting OTA URL with unsupported host: %s",
                 download_url ? download_url : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA download: %s", download_url);
    const esp_app_desc_t *running = esp_app_get_description();
    const char *running_version = running ? running->version : "";
    char slot_name[8], slot_label[4];
    describe_next_slot(slot_name, sizeof(slot_name),
                       slot_label, sizeof(slot_label));
    display_show_ota_update(running_version, latest_version,
                            slot_name, slot_label);

    /* Cert bundle covers both github.com and the 302 redirect to
     * objects.githubusercontent.com; esp_http_client follows the redirect
     * automatically. The redirect target is a signed S3 URL whose path+query
     * is ~870 B; the outgoing "GET <path> HTTP/1.1\r\n" request-line built in
     * http_client_prepare_first_line() lives in the TX buffer
     * (buffer_size_tx, default 512 B) and overflows with the generic
     * "HTTP_CLIENT: Out of buffer" error before any payload byte is read.
     * 4096 gives plenty of headroom for variance in GitHub's signed URLs.
     * buffer_size is the RX buffer; not the bottleneck here, but bumped to
     * 4096 for symmetry. */
    esp_http_client_config_t http_cfg = {
        .url = download_url,
        .timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "OTA install succeeded; rebooting into new image");
    s_ota_skip_cycles = 0;
    /* The next boot will run as PENDING_VERIFY; rollback validation is
     * gated on a successful fetch+render in the new image. */
    esp_restart();
    return ESP_OK;  /* not reached */
}

esp_err_t ota_client_maybe_update(const dash_config_v2_t *cfg,
                                  int network_idx,
                                  int api_idx)
{
    if (!cfg || network_idx < 0 || network_idx >= cfg->network_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (api_idx < 0 || api_idx >= MAX_APIS_PER_NETWORK) {
        return ESP_ERR_INVALID_ARG;
    }

    if (running_image_pending_verify()) {
        ESP_LOGI(TAG, "Skipping OTA while running image is pending verify");
        return ESP_OK;
    }

    if (s_ota_skip_cycles > 0) {
        ESP_LOGI(TAG, "OTA throttled (%u cycles remaining)",
                 (unsigned)s_ota_skip_cycles);
        s_ota_skip_cycles--;
        return ESP_OK;
    }

    const dash_wifi_profile_t *net = &cfg->networks[network_idx];
    if (api_idx >= (int)net->api_count) return ESP_ERR_INVALID_ARG;
    const dash_api_profile_t *api = &net->apis[api_idx];
    if (!api->enabled || api->api_url[0] == '\0') return ESP_ERR_INVALID_STATE;

    char latest_version[40] = {0};
    char download_url[256]  = {0};
    bool ota_enabled = false;
    esp_err_t err = fetch_manifest(api->api_url, api->device_token,
                                   latest_version, sizeof(latest_version),
                                   download_url, sizeof(download_url),
                                   &ota_enabled);
    if (err != ESP_OK) {
        s_ota_skip_cycles = CONFIG_DEVDASH_OTA_THROTTLE_CYCLES;
        ESP_LOGW(TAG, "Throttling OTA for %u cycles after manifest failure",
                 (unsigned)s_ota_skip_cycles);
        return err;
    }
    if (!ota_enabled) {
        ESP_LOGI(TAG, "OTA disabled by API; skipping");
        return ESP_OK;
    }
    if (latest_version[0] == '\0' || download_url[0] == '\0') {
        ESP_LOGW(TAG, "Manifest enabled but missing fields; skipping");
        return ESP_OK;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    const char *running_version = running ? running->version : "";
    ESP_LOGI(TAG, "OTA: running=%s latest=%s", running_version, latest_version);

    if (versions_match(running_version, latest_version)) {
        ESP_LOGI(TAG, "Already on latest version; nothing to do");
        return ESP_OK;
    }

    err = download_and_install(download_url, latest_version);
    if (err != ESP_OK) {
        s_ota_skip_cycles = CONFIG_DEVDASH_OTA_THROTTLE_CYCLES;
        ESP_LOGW(TAG, "Throttling OTA for %u cycles after download failure",
                 (unsigned)s_ota_skip_cycles);
    }
    return err;
}

void ota_client_mark_image_valid(void)
{
    if (!running_image_pending_verify()) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image marked VALID; rollback cancelled");
    } else {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s",
                 esp_err_to_name(err));
    }
}
