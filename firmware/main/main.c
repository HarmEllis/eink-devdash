#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "storage.h"
#include "wifi_prov.h"
#include "api_client.h"
#include "display.h"

static const char *TAG = "main";

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

    if (!cfg.provisioned) {
        ESP_LOGI(TAG, "No config — starting provisioning");
        display_show_qr();
        wifi_prov_run(&cfg);
        storage_save(&cfg);
    }

    dashboard_data_t data = {0};
    if (api_client_fetch(&cfg, &data) == ESP_OK) {
        display_render(&data, &cfg);
    } else {
        ESP_LOGW(TAG, "API fetch failed");
        display_show_offline();
    }

    uint32_t sleep_sec = cfg.refresh_min * 60;
    ESP_LOGI(TAG, "Deep sleep %lus", (unsigned long)sleep_sec);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
    esp_deep_sleep_start();
}
