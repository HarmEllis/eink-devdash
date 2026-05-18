#include "wifi_roam.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "wifi_roam";

#define FAST_PATH_TIMEOUT_MS 3000
#define CONNECT_TIMEOUT_MS 8000
#define MAX_SCAN_APS 32

typedef struct {
    uint8_t idx;
    int8_t rssi;
} candidate_t;

static esp_err_t wait_for_ip(int timeout_ms)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (sta) {
            esp_netif_ip_info_t ip = {0};
            if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t connect_one(const dash_wifi_profile_t *net, int timeout_ms)
{
    if (!net->enabled || net->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, net->ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, net->password,
            sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Connecting to configured SSID: %s", net->ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;

    err = wait_for_ip(timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect timeout for SSID: %s", net->ssid);
    }
    return err;
}

static int find_scanned_rssi(const wifi_ap_record_t *aps, uint16_t ap_count,
                             const char *ssid)
{
    bool found = false;
    int best = -128;
    for (uint16_t i = 0; i < ap_count; i++) {
        if (strncmp((const char *)aps[i].ssid, ssid, sizeof(aps[i].ssid)) == 0) {
            if (!found || aps[i].rssi > best) best = aps[i].rssi;
            found = true;
        }
    }
    return found ? best : -128;
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

esp_err_t wifi_roam_connect(dash_config_v2_t *cfg, int *network_idx_out)
{
    if (network_idx_out) *network_idx_out = -1;
    if (!cfg || cfg->network_count == 0) return ESP_ERR_NOT_FOUND;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_NOT_STOPPED) err = ESP_OK;
    ESP_ERROR_CHECK(err);

    if (cfg->last_success_network_idx >= 0 &&
        cfg->last_success_network_idx < (int8_t)cfg->network_count) {
        uint8_t idx = (uint8_t)cfg->last_success_network_idx;
        err = connect_one(&cfg->networks[idx], FAST_PATH_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (network_idx_out) *network_idx_out = idx;
            return ESP_OK;
        }
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_LOGI(TAG, "Scanning for configured WiFi networks");
    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) return err;

    wifi_ap_record_t aps[MAX_SCAN_APS] = {0};
    uint16_t ap_count = MAX_SCAN_APS;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, aps));

    candidate_t candidates[MAX_WIFI_NETWORKS] = {0};
    uint8_t candidate_count = 0;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        const dash_wifi_profile_t *net = &cfg->networks[i];
        if (!net->enabled || net->ssid[0] == '\0') continue;
        int rssi = find_scanned_rssi(aps, ap_count, net->ssid);
        if (rssi == -128) continue;
        candidates[candidate_count++] = (candidate_t){ .idx = i, .rssi = rssi };
    }
    sort_candidates(candidates, candidate_count);

    for (uint8_t i = 0; i < candidate_count; i++) {
        uint8_t idx = candidates[i].idx;
        ESP_LOGI(TAG, "Trying SSID %s (RSSI %d)",
                 cfg->networks[idx].ssid, candidates[i].rssi);
        err = connect_one(&cfg->networks[idx], CONNECT_TIMEOUT_MS);
        if (err == ESP_OK) {
            cfg->last_success_network_idx = idx;
            storage_save_v2(cfg);
            if (network_idx_out) *network_idx_out = idx;
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "No configured WiFi network connected");
    return ESP_ERR_NOT_FOUND;
}
