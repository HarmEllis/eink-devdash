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

esp_err_t api_client_fetch(const dash_config_t *cfg, dashboard_data_t *out)
{
    memset(out, 0, sizeof(*out));

    char url[320];
    /* api_url is a base like "http://192.168.1.50:3000"; append /dashboard. */
    size_t base_len = strlen(cfg->api_url);
    bool has_slash = base_len > 0 && cfg->api_url[base_len - 1] == '/';
    snprintf(url, sizeof(url), "%s%sdashboard",
             cfg->api_url, has_slash ? "" : "/");

    char *buf = calloc(1, RESPONSE_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    http_ctx_t ctx = { .buf = buf, .len = 0, .truncated = false };

    esp_http_client_config_t hcfg = {
        .url            = url,
        .event_handler  = http_event_handler,
        .user_data      = &ctx,
        .timeout_ms     = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        free(buf);
        out->offline = true;
        return ESP_FAIL;
    }

    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->device_token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP fetch failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        free(buf);
        out->offline = true;
        return ESP_FAIL;
    }

    if (ctx.truncated) {
        ESP_LOGW(TAG, "response truncated at %d bytes (buffer=%d)",
                 ctx.len, RESPONSE_BUF_SIZE);
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        out->offline = true;
        return ESP_FAIL;
    }

    out->schema_version = json_int(root, "schemaVersion");

    cJSON *gh = cJSON_GetObjectItemCaseSensitive(root, "github");
    out->github.issues     = json_int(gh, "issues");
    out->github.prs        = json_int(gh, "prs");
    out->github.dependabot = json_int(gh, "dependabot");

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
    out->codex.daily_used  = json_int(cx, "dailyUsed");
    out->codex.daily_limit = json_int(cx, "dailyLimit");

    cJSON *ua = cJSON_GetObjectItemCaseSensitive(root, "updatedAt");
    if (ua && cJSON_IsString(ua))
        copy_updated_at(out->updated_at, sizeof(out->updated_at), ua->valuestring);

    out->stale   = json_bool(root, "stale");
    out->offline = false;

    cJSON_Delete(root);
    return ESP_OK;
}
