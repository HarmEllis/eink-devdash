#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_prov.h"
#include "api_client.h"
#include "display.h"
#include <string.h>

static const char *TAG = "main";

static void enter_deep_sleep(uint8_t minutes)
{
    if (minutes < 3)  minutes = 3;
    if (minutes > 60) minutes = 60;
    uint64_t us = (uint64_t)minutes * 60ULL * 1000000ULL;
    ESP_LOGI(TAG, "Deep sleep %u min", minutes);
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    storage_init();
    dash_config_t cfg = {0};
    storage_load(&cfg);

    ESP_LOGI(TAG, "Boot: api=%s refresh=%u",
             cfg.api_url, cfg.refresh_min);

    /* WiFi: init, provision on first boot, then check token, then connect. */
    ESP_ERROR_CHECK(wifi_net_init());

    if (!wifi_net_is_provisioned()) {
        char prov_ssid[32], prov_pop[16];
        wifi_net_get_prov_info(prov_ssid, sizeof(prov_ssid),
                               prov_pop, sizeof(prov_pop));
        display_show_qr(prov_ssid, prov_pop);
    }

    err = wifi_net_provision_if_needed();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning failed/timeout");
        display_show_offline();
        wifi_net_stop();
        enter_deep_sleep(cfg.refresh_min);
        return;
    }

    if (cfg.device_token[0] == '\0') {
        ESP_LOGE(TAG, "No device token configured (set via menuconfig)");
        display_show_offline();
        wifi_net_stop();
        enter_deep_sleep(cfg.refresh_min);
        return;
    }

    err = wifi_net_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed, attempting reprovisioning");
        /* SSID/password may have changed since last provisioning. Try once
         * more by forcing a new provisioning round before giving up. */
        wifi_net_stop();
        if (wifi_net_forget() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear stored credentials; giving up");
            display_show_offline();
            enter_deep_sleep(cfg.refresh_min);
            return;
        }
        err = wifi_net_provision_if_needed();
        if (err == ESP_OK) err = wifi_net_connect();
        if (err != ESP_OK) {
            display_show_offline();
            wifi_net_stop();
            enter_deep_sleep(cfg.refresh_min);
            return;
        }
    }

    dashboard_data_t data = {0};
    err = api_client_fetch(&cfg, &data);
    wifi_net_stop();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "API fetch failed, showing offline");
        display_show_offline();
    } else {
        display_render(&data);
    }

    enter_deep_sleep(cfg.refresh_min);
}
