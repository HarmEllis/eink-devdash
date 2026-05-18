#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_prov.h"
#include "wifi_roam.h"
#include "api_client.h"
#include "display.h"
#include <string.h>

static const char *TAG = "main";
#define BOOT_WAKE_GPIO GPIO_NUM_0

static bool boot_button_pressed(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_WAKE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(50));
    return gpio_get_level(BOOT_WAKE_GPIO) == 0;
}

static void enter_deep_sleep(uint8_t minutes)
{
    if (minutes < 3)  minutes = 3;
    if (minutes > 60) minutes = 60;
    uint64_t us = (uint64_t)minutes * 60ULL * 1000000ULL;
    ESP_LOGI(TAG, "Deep sleep %u min", minutes);
    esp_sleep_enable_timer_wakeup(us);
    gpio_pullup_en(BOOT_WAKE_GPIO);
    esp_sleep_enable_ext0_wakeup(BOOT_WAKE_GPIO, 0);
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

    ESP_ERROR_CHECK(wifi_net_init());
    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);

    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    bool boot_wake = wake == ESP_SLEEP_WAKEUP_EXT0 && boot_button_pressed();

    ESP_LOGI(TAG, "Boot: networks=%u refresh=%u wake=%d boot_button=%d",
             cfg.network_count, cfg.refresh_min, wake, boot_wake);

    if (boot_wake || cfg.network_count == 0) {
        char prov_ssid[32], prov_pop[16];
        wifi_net_get_prov_info(prov_ssid, sizeof(prov_ssid),
                               prov_pop, sizeof(prov_pop));
        display_show_qr(prov_ssid, prov_pop);

        err = boot_wake ? wifi_net_open_config_window()
                        : wifi_net_provision_if_needed();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning/config window failed or timed out");
            display_show_offline();
            wifi_net_stop();
            enter_deep_sleep(cfg.refresh_min);
            return;
        }
        storage_seed_current_sta_if_empty(&cfg);
        storage_load_v2(&cfg);
        esp_restart();
    }

    int network_idx = -1;
    err = wifi_roam_connect(&cfg, &network_idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No configured WiFi network available");
        display_show_offline();
        wifi_net_stop();
        enter_deep_sleep(cfg.refresh_min);
        return;
    }

    dashboard_data_t data = {0};
    int api_idx = -1;
    err = api_client_fetch_with_failover(&cfg, network_idx, &data, &api_idx);
    wifi_net_stop();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "API fetch failed, showing offline");
        display_show_offline();
    } else {
        display_render(&data);
    }

    enter_deep_sleep(cfg.refresh_min);
}
