#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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
    cfg.provisioned = true;  /* skip wifi provisioning during layout test */

    /* Static demo data — V3 layout test (normal state, no alerts) */
    dashboard_data_t data = {
        .schema_version = 1,
        .github = { .issues = 12, .prs = 4, .dependabot = 0 },
        .claude = {
            .five_hour = { .used = 64, .limit = 100 },
            .weekly    = { .used = 41, .limit = 100 },
            .auth_error = false,
        },
        .codex = { .daily_used = 52, .daily_limit = 100 },
        .updated_at = "14:32",
        .stale   = false,
        .offline = false,
    };
    display_render(&data);

    /* Deep sleep disabled during layout test — just halt */
    ESP_LOGI(TAG, "Layout test done, halting.");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
