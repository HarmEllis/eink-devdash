#include "api_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_client";
#define RESPONSE_BUF_SIZE 4096

typedef struct {
    char *buf;
    int  len;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int remaining = RESPONSE_BUF_SIZE - ctx->len - 1;
        int copy = evt->data_len < remaining ? evt->data_len : remaining;
        memcpy(ctx->buf + ctx->len, evt->data, copy);
        ctx->len += copy;
    }
    return ESP_OK;
}

esp_err_t api_client_fetch(const dash_config_t *cfg, dashboard_data_t *out)
{
    char *buf = calloc(1, RESPONSE_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    http_ctx_t ctx = { .buf = buf, .len = 0 };

    esp_http_client_config_t hcfg = {
        .url            = cfg->api_url,
        .event_handler  = http_event_handler,
        .user_data      = &ctx,
        .timeout_ms     = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);

    char auth[80];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->device_token);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP fetch failed: %s", esp_err_to_name(err));
        free(buf);
        out->offline = true;
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    out->schema_version = cJSON_GetObjectItem(root, "schema_version") ?
        cJSON_GetObjectItem(root, "schema_version")->valueint : 0;

    cJSON *gh = cJSON_GetObjectItem(root, "github");
    if (gh) {
        out->github.issues    = cJSON_GetObjectItem(gh, "issues")    ? cJSON_GetObjectItem(gh, "issues")->valueint    : 0;
        out->github.prs       = cJSON_GetObjectItem(gh, "prs")       ? cJSON_GetObjectItem(gh, "prs")->valueint       : 0;
        out->github.dependabot = cJSON_GetObjectItem(gh, "dependabot") ? cJSON_GetObjectItem(gh, "dependabot")->valueint : 0;
    }

    cJSON *cl = cJSON_GetObjectItem(root, "claude");
    if (cl) {
        cJSON *fh = cJSON_GetObjectItem(cl, "five_hour");
        if (fh) {
            out->claude.five_hour.used           = cJSON_GetObjectItem(fh, "used")           ? cJSON_GetObjectItem(fh, "used")->valueint           : 0;
            out->claude.five_hour.limit          = cJSON_GetObjectItem(fh, "limit")          ? cJSON_GetObjectItem(fh, "limit")->valueint          : 0;
            out->claude.five_hour.reset_in_seconds = cJSON_GetObjectItem(fh, "reset_in_seconds") ? cJSON_GetObjectItem(fh, "reset_in_seconds")->valueint : 0;
        }
        cJSON *wk = cJSON_GetObjectItem(cl, "weekly");
        if (wk) {
            out->claude.weekly.used            = cJSON_GetObjectItem(wk, "used")            ? cJSON_GetObjectItem(wk, "used")->valueint            : 0;
            out->claude.weekly.limit           = cJSON_GetObjectItem(wk, "limit")           ? cJSON_GetObjectItem(wk, "limit")->valueint           : 0;
            out->claude.weekly.reset_in_seconds  = cJSON_GetObjectItem(wk, "reset_in_seconds")  ? cJSON_GetObjectItem(wk, "reset_in_seconds")->valueint  : 0;
        }
        out->claude.auth_error = cJSON_IsTrue(cJSON_GetObjectItem(cl, "auth_error"));
    }

    cJSON *cx = cJSON_GetObjectItem(root, "codex");
    if (cx) {
        out->codex.daily_used  = cJSON_GetObjectItem(cx, "daily_used")  ? cJSON_GetObjectItem(cx, "daily_used")->valueint  : 0;
        out->codex.daily_limit = cJSON_GetObjectItem(cx, "daily_limit") ? cJSON_GetObjectItem(cx, "daily_limit")->valueint : 0;
    }

    cJSON *ua = cJSON_GetObjectItem(root, "updated_at");
    if (ua && ua->valuestring)
        strncpy(out->updated_at, ua->valuestring, sizeof(out->updated_at) - 1);

    out->stale   = cJSON_IsTrue(cJSON_GetObjectItem(root, "stale"));
    out->offline = false;

    cJSON_Delete(root);
    return ESP_OK;
}
