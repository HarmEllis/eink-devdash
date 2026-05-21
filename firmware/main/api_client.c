#include "api_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "api_client";
#define RESPONSE_BUF_SIZE 4096

typedef struct {
    char *buf;
    int  len;
    bool truncated;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int remaining = RESPONSE_BUF_SIZE - ctx->len - 1;
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

static int json_int(const cJSON *obj, const char *key)
{
    if (!obj) return 0;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : 0;
}

static bool json_bool(const cJSON *obj, const char *key)
{
    if (!obj) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return v && cJSON_IsTrue(v);
}

static double json_double(const cJSON *obj, const char *key)
{
    if (!obj) return 0.0;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : 0.0;
}

static int round_percent(double value)
{
    if (value < 0.0) return 0;
    if (value > 100.0) return 100;
    return (int)(value + 0.5);
}

/* Extract "HH:MM" from an ISO-8601 timestamp like "2026-05-16T14:32:00Z".
 * Falls back to copying as-is when the input is shorter / not ISO. */
static void copy_updated_at(char *dst, size_t dst_sz, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= 16 && src[10] == 'T') {
        snprintf(dst, dst_sz, "%c%c:%c%c",
                 src[11], src[12], src[14], src[15]);
    } else {
        strncpy(dst, src, dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    }
}

static esp_err_t parse_dashboard_json(const char *buf, dashboard_data_t *out)
{
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        out->offline = true;
        return ESP_FAIL;
    }

    out->schema_version = json_int(root, "schemaVersion");

    cJSON *gh = cJSON_GetObjectItemCaseSensitive(root, "github");
    out->github_present = gh && cJSON_IsObject(gh);
    out->github.issues     = json_int(gh, "issues");
    out->github.prs        = json_int(gh, "prs");
    out->github.dependabot = json_int(gh, "dependabot");
    out->github.auth_error = json_bool(gh, "authError");

    cJSON *cl = cJSON_GetObjectItemCaseSensitive(root, "claude");
    if (cl) {
        cJSON *fh = cJSON_GetObjectItemCaseSensitive(cl, "fiveHour");
        out->claude.five_hour.used             = json_int(fh, "used");
        out->claude.five_hour.limit            = json_int(fh, "limit");
        out->claude.five_hour.reset_in_seconds = json_int(fh, "resetInSeconds");

        cJSON *wk = cJSON_GetObjectItemCaseSensitive(cl, "weekly");
        out->claude.weekly.used             = json_int(wk, "used");
        out->claude.weekly.limit            = json_int(wk, "limit");
        out->claude.weekly.reset_in_seconds = json_int(wk, "resetInSeconds");

        out->claude.auth_error = json_bool(cl, "authError");
    }

    cJSON *cx = cJSON_GetObjectItemCaseSensitive(root, "codex");
    if (cx) {
        cJSON *short_window = cJSON_GetObjectItemCaseSensitive(cx, "short");
        cJSON *long_window  = cJSON_GetObjectItemCaseSensitive(cx, "long");
        cJSON *reached      = cJSON_GetObjectItemCaseSensitive(cx, "reachedLimit");

        out->codex.short_pct = round_percent(json_double(short_window, "usedPercent"));
        out->codex.long_pct  = round_percent(json_double(long_window,  "usedPercent"));
        out->codex.short_reset_in_seconds = json_int(short_window, "resetInSeconds");
        out->codex.long_reset_in_seconds  = json_int(long_window,  "resetInSeconds");
        out->codex.reached   = reached && cJSON_IsString(reached);
    }

    cJSON *ua = cJSON_GetObjectItemCaseSensitive(root, "updatedAtLocal");
    if (!ua) {
        ua = cJSON_GetObjectItemCaseSensitive(root, "updatedAt");
    }
    if (ua && cJSON_IsString(ua)) {
        copy_updated_at(out->updated_at, sizeof(out->updated_at), ua->valuestring);
    }

    out->stale   = json_bool(root, "stale");
    out->offline = false;

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_one(const char *base_url, const char *token,
                           dashboard_data_t *out, int *status_out)
{
    if (status_out) *status_out = 0;
    char *buf = calloc(1, RESPONSE_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    http_ctx_t ctx = { .buf = buf, .len = 0, .truncated = false };

    char url[256];
    size_t base_len = strlen(base_url);
    bool has_slash = base_len > 0 && base_url[base_len - 1] == '/';
    snprintf(url, sizeof(url), "%s%sdashboard", base_url, has_slash ? "" : "/");

    esp_http_client_config_t hcfg = {
        .url            = url,
        .event_handler  = http_event_handler,
        .user_data      = &ctx,
        .timeout_ms     = 6000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        free(buf);
        out->offline = true;
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
    if (status_out) *status_out = status;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP fetch failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status, url);
        free(buf);
        out->offline = true;
        return err == ESP_OK ? ESP_FAIL : err;
    }

    if (ctx.truncated) {
        ESP_LOGW(TAG, "response truncated at %d bytes (buffer=%d)",
                 ctx.len, RESPONSE_BUF_SIZE);
    }

    err = parse_dashboard_json(buf, out);
    free(buf);
    return err;
}

static bool should_fail_over(int status, esp_err_t err)
{
    if (err != ESP_OK) return true;
    if (status == 401 || status == 403) return false;
    if (status == 404 || status == 429 || status == 503) return true;
    if (status >= 500 && status <= 599) return true;
    return false;
}

esp_err_t api_client_fetch_with_failover(dash_config_v2_t *cfg,
                                         int network_idx,
                                         dashboard_data_t *out,
                                         int *api_used_idx)
{
    memset(out, 0, sizeof(*out));
    if (api_used_idx) *api_used_idx = -1;
    if (!cfg || network_idx < 0 || network_idx >= cfg->network_count) {
        out->offline = true;
        return ESP_ERR_INVALID_ARG;
    }

    dash_wifi_profile_t *net = &cfg->networks[network_idx];
    if (net->api_count == 0) {
        out->offline = true;
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t last_err = ESP_FAIL;
    int start = -1;
    if (cfg->last_success_api_idx >= 0 &&
        cfg->last_success_api_idx < (int8_t)net->api_count) {
        start = cfg->last_success_api_idx;
    }

    for (uint8_t pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < net->api_count; i++) {
            uint8_t idx = (pass == 0 && start >= 0) ? (uint8_t)start : i;
            if (pass == 0 && start < 0) continue;
            if (pass == 1 && idx == start) continue;

            const dash_api_profile_t *api = &net->apis[idx];
            if (!api->enabled || api->api_url[0] == '\0') continue;
            if (api->device_token[0] == '\0') {
                ESP_LOGW(TAG, "API token empty for %s; device stays offline", api->api_url);
                out->offline = true;
                return ESP_ERR_INVALID_STATE;
            }

            int status = 0;
            last_err = fetch_one(api->api_url, api->device_token, out, &status);
            if (last_err == ESP_OK) {
                cfg->last_success_network_idx = network_idx;
                cfg->last_success_api_idx = idx;
                storage_save_v2(cfg);
                if (api_used_idx) *api_used_idx = idx;
                return ESP_OK;
            }
            if (!should_fail_over(status, last_err)) {
                ESP_LOGW(TAG, "Not failing over after auth status %d", status);
                return last_err;
            }

            if (pass == 0) break;
        }
    }

    out->offline = true;
    return last_err;
}
