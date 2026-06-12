#include "wifi_roam.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_roam";

#define DISCONNECT_RETRY_DELAY_MIN_MS 1000
#define DISCONNECT_RETRY_DELAY_MAX_MS 8000
#define DISCONNECT_SETTLE_MS 500
#define DRIVER_FAILURE_RETRY_COUNT 3
#define MAX_SCAN_APS 32

/* When the first active scan sees none of the configured networks, re-scan
 * once after a short back-off to absorb a missed beacon. */
#define WIFI_SCAN_MAX_TRIES 2
#define WIFI_SCAN_RETRY_DELAY_MS 400

typedef struct {
    uint8_t idx;
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
} candidate_t;

typedef struct {
    volatile bool associated;
    volatile bool disconnected;
    volatile bool auth_error_seen;
    volatile uint8_t disconnect_reason;
    volatile TickType_t disconnected_at;
} connect_state_t;

static void diag_set_reason(wifi_unreachable_diag_t *diag,
                            uint8_t network_idx,
                            const char *reason)
{
    if (!diag || !reason) return;
    for (uint8_t i = 0; i < diag->row_count; i++) {
        if (diag->rows[i].network_idx == network_idx) {
            strlcpy(diag->rows[i].reason, reason,
                    sizeof(diag->rows[i].reason));
            return;
        }
    }
    if (diag->row_count >= MAX_WIFI_NETWORKS) return;
    wifi_unreachable_row_t *row = &diag->rows[diag->row_count++];
    row->network_idx = network_idx;
    strlcpy(row->reason, reason, sizeof(row->reason));
}

static bool wifi_reason_is_auth_error(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return true;
    default:
        return false;
    }
}

static const char *wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:       return "auth-expire";
    case WIFI_REASON_AUTH_FAIL:         return "auth-fail";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                                            return "4way-timeout";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake-timeout";
    case WIFI_REASON_NO_AP_FOUND:       return "no-ap";
    case WIFI_REASON_CONNECTION_FAIL:   return "connection-fail";
    default:                            return "other";
    }
}

static const char *connect_failure_reason(const connect_state_t *state,
                                          esp_err_t err,
                                          bool candidate_seen)
{
    if (!candidate_seen) return "no-ap";
    if (state && state->disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
        return "no-ap";
    }
    if (state && (state->auth_error_seen ||
                  wifi_reason_is_auth_error(state->disconnect_reason))) {
        return "auth-err";
    }
    if (err == ESP_ERR_TIMEOUT || (state && state->associated)) {
        return "timeout";
    }
    return "timeout";
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    connect_state_t *state = (connect_state_t *)arg;
    if (!state || event_base != WIFI_EVENT) return;

    if (event_id == WIFI_EVENT_STA_CONNECTED) {
        state->associated = true;
        state->disconnected = false;
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;
        state->associated = false;
        state->disconnected = true;
        state->disconnect_reason = event ? event->reason : 0;
        if (event && wifi_reason_is_auth_error(event->reason)) {
            state->auth_error_seen = true;
        }
        state->disconnected_at = xTaskGetTickCount();
        ESP_LOGI(TAG,
                 "WiFi disconnected: reason=%u (%s) auth_error_seen=%d",
                 state->disconnect_reason,
                 wifi_reason_name(state->disconnect_reason),
                 state->auth_error_seen);
    }
}

static bool sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            return true;
        }
    }
    return false;
}

static TickType_t retry_delay_ticks(uint8_t retry_count)
{
    uint32_t delay_ms = DISCONNECT_RETRY_DELAY_MIN_MS;
    while (retry_count-- > 0 && delay_ms < DISCONNECT_RETRY_DELAY_MAX_MS) {
        delay_ms *= 2;
    }
    if (delay_ms > DISCONNECT_RETRY_DELAY_MAX_MS) {
        delay_ms = DISCONNECT_RETRY_DELAY_MAX_MS;
    }
    return pdMS_TO_TICKS(delay_ms);
}

static esp_err_t wait_for_ip_with_retries(const char *ssid, int timeout_ms,
                                          connect_state_t *state)
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    uint8_t retry_count = 0;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (sta_has_ip()) return ESP_OK;

        TickType_t now = xTaskGetTickCount();
        if (state->disconnected &&
            (now - state->disconnected_at) >= retry_delay_ticks(retry_count)) {
            ESP_LOGI(TAG, "Disconnected from SSID %s (reason %u); retrying connect",
                     ssid, state->disconnect_reason);
            state->disconnected = false;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
            if (retry_count < 8) retry_count++;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

static void cancel_connect_attempt(void)
{
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(DISCONNECT_SETTLE_MS));
}

static esp_err_t connect_one(const dash_wifi_profile_t *net, int timeout_ms,
                             const candidate_t *candidate,
                             char *reason_out,
                             size_t reason_out_sz)
{
    if (!net->enabled || net->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (reason_out && reason_out_sz > 0) reason_out[0] = '\0';

    cancel_connect_attempt();

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, net->ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, net->password,
            sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;
    wcfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wcfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wcfg.sta.failure_retry_cnt = DRIVER_FAILURE_RETRY_COUNT;
    wcfg.sta.rm_enabled = 1;
    wcfg.sta.btm_enabled = 1;
    wcfg.sta.mbo_enabled = 1;

    /* When the caller already picked a specific AP from the fresh scan in
     * wifi_roam_connect(), pin its BSSID and channel so the driver associates
     * directly instead of running a second all-channel scan of its own. That
     * scan already ranked APs by signal, so pinning the chosen BSSID preserves
     * strongest-AP selection (across multiple APs on the same SSID) while
     * dropping the redundant scan. The NULL-candidate fallback path has no
     * fresh scan result, so it keeps the all-channel scan and lets the driver
     * locate the AP on its own. */
    if (candidate) {
        wcfg.sta.bssid_set = true;
        memcpy(wcfg.sta.bssid, candidate->bssid, sizeof(wcfg.sta.bssid));
        wcfg.sta.channel = candidate->channel;
        wcfg.sta.scan_method = WIFI_FAST_SCAN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) return err;

    connect_state_t state = {0};
    esp_event_handler_instance_t event_instance = NULL;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, &state,
                                              &event_instance);
    if (err != ESP_OK) return err;

    if (candidate) {
        ESP_LOGI(TAG, "Connecting to configured SSID: %s; best scan match "
                 MACSTR " channel %u (RSSI %d)",
                 net->ssid, MAC2STR(candidate->bssid), candidate->channel,
                 candidate->rssi);
    } else {
        ESP_LOGI(TAG, "Connecting to configured SSID: %s", net->ssid);
    }
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) goto out;

    err = wait_for_ip_with_retries(net->ssid, timeout_ms, &state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect timeout for SSID: %s after %ds",
                 net->ssid, timeout_ms / 1000);
    }

out:
    if (err != ESP_OK && reason_out && reason_out_sz > 0) {
        strlcpy(reason_out,
                connect_failure_reason(&state, err, candidate != NULL),
                reason_out_sz);
    }
    if (err != ESP_OK) cancel_connect_attempt();

    if (event_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              event_instance);
    }
    return err;
}

static bool find_best_scanned_ap(const wifi_ap_record_t *aps, uint16_t ap_count,
                                 const char *ssid, candidate_t *candidate)
{
    bool found = false;
    for (uint16_t i = 0; i < ap_count; i++) {
        if (strncmp((const char *)aps[i].ssid, ssid, sizeof(aps[i].ssid)) == 0) {
            if (!found || aps[i].rssi > candidate->rssi) {
                candidate->rssi = aps[i].rssi;
                candidate->channel = aps[i].primary;
                memcpy(candidate->bssid, aps[i].bssid, sizeof(candidate->bssid));
                found = true;
            }
        }
    }
    return found;
}

static void sort_candidates(candidate_t *candidates, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if (candidates[j].rssi > candidates[i].rssi ||
                (candidates[j].rssi == candidates[i].rssi &&
                 candidates[j].idx < candidates[i].idx)) {
                candidate_t tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }
}

esp_err_t wifi_roam_connect(dash_config_v2_t *cfg,
                            int *network_idx_out,
                            wifi_unreachable_diag_t *diag)
{
    if (network_idx_out) *network_idx_out = -1;
    if (diag) memset(diag, 0, sizeof(*diag));
    if (!cfg || cfg->network_count == 0) return ESP_ERR_NOT_FOUND;

    /* Per-attempt connect budget, configured in the portal and stored in cfg
       (already normalized by storage). Re-clamp defensively: a 0/legacy/garbage
       value falls back to the default rather than producing a 0 ms timeout. */
    uint8_t to_s = cfg->wifi_connect_timeout_s;
    if (to_s < DASH_WIFI_CONNECT_TIMEOUT_MIN || to_s > DASH_WIFI_CONNECT_TIMEOUT_MAX)
        to_s = DASH_WIFI_CONNECT_TIMEOUT_DEFAULT;
    const int connect_timeout_ms = (int)to_s * 1000;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_NOT_STOPPED) err = ESP_OK;
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_LOGI(TAG, "Scanning for configured WiFi networks");

    candidate_t candidates[MAX_WIFI_NETWORKS] = {0};
    uint8_t candidate_count = 0;
    for (uint8_t scan_try = 0; scan_try < WIFI_SCAN_MAX_TRIES; scan_try++) {
        if (scan_try > 0) {
            ESP_LOGW(TAG, "No configured WiFi network seen; re-scanning after %d ms",
                     WIFI_SCAN_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_RETRY_DELAY_MS));
        }

        err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) return err;

        wifi_ap_record_t *aps = calloc(MAX_SCAN_APS, sizeof(*aps));
        if (!aps) return ESP_ERR_NO_MEM;

        uint16_t ap_count = MAX_SCAN_APS;
        err = esp_wifi_scan_get_ap_records(&ap_count, aps);
        if (err != ESP_OK) {
            free(aps);
            return err;
        }

        candidate_count = 0;
        for (uint8_t i = 0; i < cfg->network_count; i++) {
            const dash_wifi_profile_t *net = &cfg->networks[i];
            if (!net->enabled || net->ssid[0] == '\0') continue;
            candidate_t candidate = { .idx = i, .rssi = -128 };
            if (!find_best_scanned_ap(aps, ap_count, net->ssid, &candidate)) {
                diag_set_reason(diag, i, "no-ap");
                continue;
            }
            candidates[candidate_count++] = candidate;
        }
        free(aps);
        if (candidate_count > 0) break;
    }
    sort_candidates(candidates, candidate_count);

    for (uint8_t i = 0; i < candidate_count; i++) {
        uint8_t idx = candidates[i].idx;
        char reason[UNREACHABLE_REASON_MAX] = {0};
        err = connect_one(&cfg->networks[idx], connect_timeout_ms,
                          &candidates[i], reason, sizeof(reason));
        if (err == ESP_OK) {
            cfg->last_success_network_idx = idx;
            storage_note_last_success(idx, cfg->last_success_api_idx);
            if (network_idx_out) *network_idx_out = idx;
            return ESP_OK;
        }
        diag_set_reason(diag, idx, reason[0] ? reason : "timeout");
    }

    /* Fall back to the last successful network on all channels when it is not
     * in the scanned candidate set — either nothing was visible, or a missed
     * primary was hidden behind a visible secondary that we already tried.
     * (If it was scanned, the candidate loop above already attempted it.)
     * Keep this to at most one extra all-channel attempt. */
    if (cfg->last_success_network_idx >= 0 &&
        cfg->last_success_network_idx < (int8_t)cfg->network_count) {
        uint8_t idx = (uint8_t)cfg->last_success_network_idx;
        bool already_scanned = false;
        for (uint8_t i = 0; i < candidate_count; i++) {
            if (candidates[i].idx == idx) {
                already_scanned = true;
                break;
            }
        }
        if (!already_scanned) {
            ESP_LOGW(TAG, "Last successful WiFi network not in scan; trying it on all channels");
            char reason[UNREACHABLE_REASON_MAX] = {0};
            err = connect_one(&cfg->networks[idx], connect_timeout_ms, NULL,
                              reason, sizeof(reason));
            if (err == ESP_OK) {
                if (network_idx_out) *network_idx_out = idx;
                return ESP_OK;
            }
            diag_set_reason(diag, idx, reason[0] ? reason : "no-ap");
        }
    }

    /* WiFi failures are always treated as retryable: on WPA2-PSK a 4-way
       handshake timeout is indistinguishable from a wrong password versus
       packet loss, so the in-wake retry (bounded by its elapsed-time cutoff)
       handles them rather than risk wrongly suppressing a recoverable wake. */
    ESP_LOGW(TAG, "No configured WiFi network connected");
    return ESP_ERR_NOT_FOUND;
}
