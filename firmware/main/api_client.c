#include "api_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "runtime_policy.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "api_client";
/* Headroom for schemaVersion 2 services plus future optional metrics. The
 * parser still rejects truncated responses before JSON parsing. */
#define RESPONSE_BUF_SIZE 6144
#define DASHBOARD_HTTP_TIMEOUT_MS 10000
#define RELAY_HTTP_TIMEOUT_MS 22000
#define DASHBOARD_FETCH_CYCLE_BUDGET_MS 60000
#define DASHBOARD_HTTP_ATTEMPTS 3
#define DASHBOARD_HTTP_RETRY_DELAY_MS 750

typedef struct {
    char *buf;
    int  len;
    bool truncated;
} http_ctx_t;

static void diag_set_reason(api_unreachable_diag_t *diag,
                            uint8_t api_idx,
                            const char *reason)
{
    if (!diag || !reason) return;
    for (uint8_t i = 0; i < diag->row_count; i++) {
        if (diag->rows[i].api_idx == api_idx) {
            strlcpy(diag->rows[i].reason, reason,
                    sizeof(diag->rows[i].reason));
            return;
        }
    }
    if (diag->row_count >= MAX_APIS_PER_NETWORK) return;
    api_unreachable_row_t *row = &diag->rows[diag->row_count++];
    row->api_idx = api_idx;
    strlcpy(row->reason, reason, sizeof(row->reason));
}

static bool url_host_is_ip_literal(const char *url)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    if (!host || !host[0]) return false;

    if (host[0] == '[') {
        const char *end = strchr(host, ']');
        return end && end > host + 1;
    }

    bool has_digit = false;
    bool has_dot = false;
    for (const char *p = host; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        if (*p == ':') break;
        if (*p >= '0' && *p <= '9') {
            has_digit = true;
            continue;
        }
        if (*p == '.') {
            has_dot = true;
            continue;
        }
        return false;
    }
    return has_digit && has_dot;
}

static const char *http_failure_reason(const char *base_url,
                                       esp_err_t err,
                                       int status,
                                       int sock_errno,
                                       char *buf,
                                       size_t buf_sz)
{
    if (status > 0) {
        snprintf(buf, buf_sz, "%d", status);
        return buf;
    }
    if (sock_errno == ECONNREFUSED || sock_errno == ECONNRESET) {
        return "refused";
    }
    if (sock_errno == ETIMEDOUT || sock_errno == EAGAIN ||
        err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN) {
        return "timeout";
    }
    /* esp-tls select() timeouts are surfaced by esp_http_client_perform() as
     * ESP_ERR_HTTP_CONNECT with no socket errno in the IP-literal case. Keep
     * hostname failures generic because DNS/connect failures share this code. */
    if (err == ESP_ERR_HTTP_CONNECT && sock_errno == 0) {
        return url_host_is_ip_literal(base_url) ? "timeout" : "err";
    }
    return "timeout";
}

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

static const char *json_string(const cJSON *obj, const char *key)
{
    if (!obj) return NULL;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static bool json_string_equals(const cJSON *obj, const char *key,
                               const char *expected)
{
    const char *value = json_string(obj, key);
    return value && strcmp(value, expected) == 0;
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

static const cJSON *find_service(const cJSON *root, const char *id)
{
    const cJSON *services = cJSON_GetObjectItemCaseSensitive(root, "services");
    if (!services || !cJSON_IsArray(services)) return NULL;

    const cJSON *service = NULL;
    cJSON_ArrayForEach(service, services) {
        if (cJSON_IsObject(service) && json_string_equals(service, "id", id)) {
            return service;
        }
    }
    return NULL;
}

static const cJSON *find_service_item(const cJSON *service,
                                      const char *array_key,
                                      const char *id)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(service, array_key);
    if (!items || !cJSON_IsArray(items)) return NULL;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (cJSON_IsObject(item) && json_string_equals(item, "id", id)) {
            return item;
        }
    }
    return NULL;
}

static int service_counter_value(const cJSON *service, const char *id)
{
    const cJSON *counter = find_service_item(service, "counters", id);
    return json_int(counter, "value");
}

static bool service_counter_present(const cJSON *service, const char *id)
{
    return find_service_item(service, "counters", id) != NULL;
}

/* Accept an API `valueText` only when it is a short ASCII amount: digits with at
   most one '.'/',' decimal separator, <= 15 bytes. Anything else (overlong,
   non-ASCII, malformed) is rejected so the device falls back to formatting the
   numeric `value` locally rather than drawing a garbled/truncated string. */
static bool valid_amount_text(const char *s)
{
    if (!s || s[0] == '\0') return false;
    size_t len = strlen(s);
    if (len > 15) return false;
    bool seen_sep = false, seen_frac_digit = false, seen_int_digit = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (seen_sep) seen_frac_digit = true; else seen_int_digit = true;
        } else if (c == '.' || c == ',') {
            if (seen_sep || !seen_int_digit) return false;  /* one sep, after a digit */
            seen_sep = true;
        } else {
            return false;
        }
    }
    return seen_int_digit && (!seen_sep || seen_frac_digit);
}

/* Parse the optional `extraUsage` metric into the shared spend struct. Absent
   metric → not present. `usedPercent` may be omitted (env override) → the
   device renders an amount-capped bar instead. `valueText` is the API's
   preformatted, locale-aware amount; when valid the device draws it verbatim,
   otherwise it formats `value` itself. The legacy `spend` metric is
   intentionally not read; this is its currency-aware replacement. */
static void parse_extra_usage(const cJSON *service, extra_usage_t *out)
{
    memset(out, 0, sizeof(*out));
    const cJSON *metric = find_service_item(service, "metrics", "extraUsage");
    if (!metric) return;

    out->present = true;
    out->amount = json_double(metric, "value");

    const cJSON *pct = cJSON_GetObjectItemCaseSensitive(metric, "usedPercent");
    if (pct && cJSON_IsNumber(pct)) {
        out->percent = round_percent(pct->valuedouble);
        out->percent_present = true;
    }

    const char *unit = json_string(metric, "unit");
    if (unit) {
        strncpy(out->currency, unit, sizeof(out->currency) - 1);
        out->currency[sizeof(out->currency) - 1] = '\0';
    }

    const char *vt = json_string(metric, "valueText");
    if (valid_amount_text(vt)) {
        strncpy(out->value_text, vt, sizeof(out->value_text) - 1);
        out->value_text[sizeof(out->value_text) - 1] = '\0';
    }
}

static const cJSON *service_window(const cJSON *service, const char *id)
{
    return find_service_item(service, "windows", id);
}

static bool service_auth_error(const cJSON *service)
{
    return json_string_equals(service, "status", "auth_error");
}

static bool service_error(const cJSON *service)
{
    return json_string_equals(service, "status", "error") ||
           json_string_equals(service, "status", "unavailable");
}

static void parse_dashboard_v2(const cJSON *root, dashboard_data_t *out)
{
    const cJSON *gh = find_service(root, "github");
    out->github_present = gh && cJSON_IsObject(gh);
    if (out->github_present) {
        out->github.issues = service_counter_value(gh, "issues");
        out->github.prs = service_counter_value(gh, "pullRequests");
        out->github.dependabot = service_counter_value(gh, "securityAlerts");
        out->github.notifications_present = service_counter_present(gh, "notifications");
        out->github.notifications = service_counter_value(gh, "notifications");
        out->github.auth_error = service_auth_error(gh);
        out->github.service_error = service_error(gh);
    }

    const cJSON *cl = find_service(root, "claude");
    if (cl) {
        const cJSON *fh = service_window(cl, "fiveHour");
        out->claude.five_hour.used             = json_int(fh, "used");
        out->claude.five_hour.limit            = json_int(fh, "limit");
        out->claude.five_hour.reset_in_seconds = json_int(fh, "resetInSeconds");

        const cJSON *wk = service_window(cl, "weekly");
        out->claude.weekly.used             = json_int(wk, "used");
        out->claude.weekly.limit            = json_int(wk, "limit");
        out->claude.weekly.reset_in_seconds = json_int(wk, "resetInSeconds");

        parse_extra_usage(cl, &out->claude.extra_usage);
        out->claude.auth_error = service_auth_error(cl);
    }

    const cJSON *cx = find_service(root, "codex");
    if (cx) {
        const cJSON *short_window = service_window(cx, "short");
        const cJSON *long_window  = service_window(cx, "long");

        out->codex.short_pct = round_percent(json_double(short_window, "usedPercent"));
        out->codex.long_pct  = round_percent(json_double(long_window,  "usedPercent"));
        out->codex.short_reset_in_seconds = json_int(short_window, "resetInSeconds");
        out->codex.long_reset_in_seconds  = json_int(long_window,  "resetInSeconds");
        out->codex.reached = json_bool(short_window, "reachedLimit") ||
                             json_bool(long_window, "reachedLimit");
        parse_extra_usage(cx, &out->codex.extra_usage);
    }
}

static void parse_dashboard_payload(const cJSON *root, dashboard_data_t *out)
{
    out->schema_version = json_int(root, "schemaVersion");
    parse_dashboard_v2(root, out);

    cJSON *ua = cJSON_GetObjectItemCaseSensitive(root, "updatedAtLocal");
    if (!ua) {
        ua = cJSON_GetObjectItemCaseSensitive(root, "updatedAt");
    }
    if (ua && cJSON_IsString(ua)) {
        copy_updated_at(out->updated_at, sizeof(out->updated_at), ua->valuestring);
    }

    /* Full local wall-clock timestamp for the RTC clock. Stored raw (no HH:MM
       truncation) so timekeep can parse the date+time. ONLY accept the local
       ISO field — never fall back to the UTC `updatedAt`: timekeep treats the
       parsed fields as local wall time and ignores any offset, so a UTC value
       would set the clock wrong by the timezone offset and shift quiet hours.
       When the field is absent (e.g. an older API), updated_at_iso stays empty,
       the RTC is left unset, and quiet hours simply does not activate until the
       API is updated — a safe degradation rather than a silent time skew. */
    cJSON *uai = cJSON_GetObjectItemCaseSensitive(root, "updatedAtLocalIso");
    if (uai && cJSON_IsString(uai)) {
        strncpy(out->updated_at_iso, uai->valuestring,
                sizeof(out->updated_at_iso) - 1);
        out->updated_at_iso[sizeof(out->updated_at_iso) - 1] = '\0';
    }

    out->stale   = json_bool(root, "stale");
    out->offline = false;
}

static esp_err_t parse_dashboard_json(const char *buf, dashboard_data_t *out)
{
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        out->offline = true;
        return ESP_FAIL;
    }

    parse_dashboard_payload(root, out);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_one(const char *base_url, const char *token,
                           dashboard_data_t *out,
                           int *status_out,
                           int *sock_errno_out,
                           int timeout_ms)
{
    if (status_out) *status_out = 0;
    if (sock_errno_out) *sock_errno_out = 0;
    char *buf = calloc(1, RESPONSE_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    http_ctx_t ctx = { .buf = buf, .len = 0, .truncated = false };

    char url[256];
    size_t base_len = strlen(base_url);
    bool has_slash = base_len > 0 && base_url[base_len - 1] == '/';
    snprintf(url, sizeof(url), "%s%sdashboard", base_url, has_slash ? "" : "/");
    ESP_LOGI(TAG, "Fetching dashboard endpoint: %s", url);

    esp_http_client_config_t hcfg = {
        .url            = url,
        .event_handler  = http_event_handler,
        .user_data      = &ctx,
        .timeout_ms     = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
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
    int sock_errno = esp_http_client_get_errno(client);
    if (status_out) *status_out = status;
    if (sock_errno_out) *sock_errno_out = sock_errno;
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Dashboard HTTP completed: err=%s status=%d sock_errno=%d bytes=%d truncated=%d",
             esp_err_to_name(err), status, sock_errno, ctx.len, ctx.truncated);

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
        free(buf);
        out->offline = true;
        return ESP_FAIL;
    }

    err = parse_dashboard_json(buf, out);
    ESP_LOGI(TAG, "Dashboard JSON parse result: %s schema=%d offline=%d stale=%d",
             esp_err_to_name(err), out->schema_version, out->offline,
             out->stale);
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

static bool should_retry_same_profile(int status, esp_err_t err)
{
    if (err == ESP_OK) return false;
    if (status <= 0) return true;
    if (status == 408 || status == 429) return true;
    if (status >= 500 && status <= 599) return true;
    return false;
}

static int fetch_cycle_remaining_ms(int64_t started_us)
{
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (elapsed_ms >= DASHBOARD_FETCH_CYCLE_BUDGET_MS) return 0;
    return DASHBOARD_FETCH_CYCLE_BUDGET_MS - (int)elapsed_ms;
}

static int profile_timeout_ms(const char *url, int remaining_ms)
{
    int timeout = api_url_is_relay(url)
        ? RELAY_HTTP_TIMEOUT_MS
        : DASHBOARD_HTTP_TIMEOUT_MS;
    return timeout < remaining_ms ? timeout : remaining_ms;
}

esp_err_t api_client_fetch_with_failover(dash_config_v2_t *cfg,
                                         int network_idx,
                                         bool prefer_last_success_api,
                                         dashboard_data_t *out,
                                         int *api_used_idx,
                                         api_unreachable_diag_t *diag)
{
    memset(out, 0, sizeof(*out));
    if (api_used_idx) *api_used_idx = -1;
    if (diag) memset(diag, 0, sizeof(*diag));
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
    int64_t cycle_started_us = esp_timer_get_time();
    int start = -1;
    if (prefer_last_success_api &&
        cfg->last_success_api_idx >= 0 &&
        cfg->last_success_api_idx < (int8_t)net->api_count) {
        start = cfg->last_success_api_idx;
    }
    ESP_LOGI(TAG, "API failover start: %s",
             start >= 0 ? "last successful profile" : "first configured profile");

    if (!prefer_last_success_api) {
        for (uint8_t attempt = 1; attempt <= DASHBOARD_HTTP_ATTEMPTS; attempt++) {
            for (uint8_t idx = 0; idx < net->api_count; idx++) {
                const dash_api_profile_t *api = &net->apis[idx];
                if (!api->enabled || api->api_url[0] == '\0') continue;
                if (api->device_token[0] == '\0') {
                    ESP_LOGW(TAG, "API token empty for %s; device stays offline", api->api_url);
                    diag_set_reason(diag, idx, "401");
                    out->offline = true;
                    return ESP_ERR_INVALID_STATE;
                }

                int status = 0;
                int sock_errno = 0;
                int remaining_ms = fetch_cycle_remaining_ms(cycle_started_us);
                if (remaining_ms <= 0) goto budget_exhausted;
                ESP_LOGI(TAG, "Trying API profile index=%u round=%u/%u url=%s",
                         idx, attempt, DASHBOARD_HTTP_ATTEMPTS, api->api_url);
                last_err = fetch_one(api->api_url, api->device_token,
                                     out, &status, &sock_errno,
                                     profile_timeout_ms(api->api_url, remaining_ms));
                if (last_err == ESP_OK) {
                    ESP_LOGI(TAG, "API profile index=%u succeeded", idx);
                    cfg->last_success_network_idx = network_idx;
                    cfg->last_success_api_idx = idx;
                    storage_note_last_success(network_idx, idx);
                    if (api_used_idx) *api_used_idx = idx;
                    return ESP_OK;
                }
                char reason[UNREACHABLE_REASON_MAX] = {0};
                diag_set_reason(diag, idx,
                                http_failure_reason(api->api_url,
                                                    last_err, status,
                                                    sock_errno, reason,
                                                    sizeof(reason)));
                if (!should_fail_over(status, last_err)) {
                    ESP_LOGW(TAG, "Not failing over after auth status %d", status);
                    return last_err;
                }
                ESP_LOGW(TAG, "API profile index=%u failed: err=%s status=%d; trying failover",
                         idx, esp_err_to_name(last_err), status);
            }

            if (attempt < DASHBOARD_HTTP_ATTEMPTS) {
                if (fetch_cycle_remaining_ms(cycle_started_us) <= 0) goto budget_exhausted;
                vTaskDelay(pdMS_TO_TICKS(DASHBOARD_HTTP_RETRY_DELAY_MS));
            }
        }

        out->offline = true;
        ESP_LOGW(TAG, "All API profiles failed; last_err=%s", esp_err_to_name(last_err));
        return last_err;
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
                diag_set_reason(diag, idx, "401");
                out->offline = true;
                return ESP_ERR_INVALID_STATE;
            }

            int status = 0;
            int sock_errno = 0;
            ESP_LOGI(TAG, "Trying API profile index=%u pass=%u url=%s",
                     idx, pass, api->api_url);
            for (uint8_t attempt = 1; attempt <= DASHBOARD_HTTP_ATTEMPTS; attempt++) {
                int remaining_ms = fetch_cycle_remaining_ms(cycle_started_us);
                if (remaining_ms <= 0) goto budget_exhausted;
                ESP_LOGI(TAG, "API profile index=%u attempt=%u/%u",
                         idx, attempt, DASHBOARD_HTTP_ATTEMPTS);
                last_err = fetch_one(api->api_url, api->device_token,
                                     out, &status, &sock_errno,
                                     profile_timeout_ms(api->api_url, remaining_ms));
                if (last_err == ESP_OK ||
                    !should_retry_same_profile(status, last_err) ||
                    attempt == DASHBOARD_HTTP_ATTEMPTS) {
                    break;
                }
                ESP_LOGW(TAG, "API profile index=%u transient failure: err=%s status=%d; retrying",
                         idx, esp_err_to_name(last_err), status);
                if (fetch_cycle_remaining_ms(cycle_started_us) <= 0) goto budget_exhausted;
                vTaskDelay(pdMS_TO_TICKS(DASHBOARD_HTTP_RETRY_DELAY_MS));
            }
            if (last_err == ESP_OK) {
                ESP_LOGI(TAG, "API profile index=%u succeeded", idx);
                cfg->last_success_network_idx = network_idx;
                cfg->last_success_api_idx = idx;
                storage_note_last_success(network_idx, idx);
                if (api_used_idx) *api_used_idx = idx;
                return ESP_OK;
            }
            char reason[UNREACHABLE_REASON_MAX] = {0};
            diag_set_reason(diag, idx,
                            http_failure_reason(api->api_url,
                                                last_err, status,
                                                sock_errno, reason,
                                                sizeof(reason)));
            if (!should_fail_over(status, last_err)) {
                ESP_LOGW(TAG, "Not failing over after auth status %d", status);
                return last_err;
            }
            ESP_LOGW(TAG, "API profile index=%u failed: err=%s status=%d; trying failover",
                     idx, esp_err_to_name(last_err), status);

            if (pass == 0) break;
        }
    }

    out->offline = true;
    ESP_LOGW(TAG, "All API profiles failed; last_err=%s", esp_err_to_name(last_err));
    return last_err;

budget_exhausted:
    out->offline = true;
    ESP_LOGW(TAG, "Dashboard fetch cycle exceeded %d ms budget",
             DASHBOARD_FETCH_CYCLE_BUDGET_MS);
    return ESP_ERR_TIMEOUT;
}
