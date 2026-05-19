#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_prov.h"
#include "wifi_roam.h"
#include "api_client.h"
#include "display.h"
#include "improv.h"
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
#if CONFIG_DEVDASH_DEMO_MODE
    /* Demo build: render a plausible sample dashboard once and idle the
     * chip. No NVS, no WiFi, no API — purely a visual stand-in to show the
     * device to other people without needing network configuration. */
    static dashboard_data_t demo = {
        .schema_version = 1,
        .github = { .issues = 7, .prs = 3, .dependabot = 2 },
        .claude = {
            .five_hour = { .used = 84, .limit = 200, .reset_in_seconds = 5400 },
            .weekly    = { .used = 1240, .limit = 5000, .reset_in_seconds = 0 },
            .auth_error = false,
        },
        .codex = { .daily_used = 92, .daily_limit = 250 },
        .updated_at = "2026-05-18 21:35",
        .stale = false,
        .offline = false,
    };
    ESP_LOGI(TAG, "DEMO mode: rendering static sample dashboard");
    display_render(&demo);
    for (;;) vTaskDelay(portMAX_DELAY);
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    storage_init();

    ESP_ERROR_CHECK(wifi_net_init());
    /* cfg is 7+ KiB. Keep it in BSS instead of on the main task stack
     * (CONFIG_ESP_MAIN_TASK_STACK_SIZE is only 3584 bytes). */
    static dash_config_v2_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    storage_load_v2(&cfg);

    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    esp_reset_reason_t reset = esp_reset_reason();
    bool boot_wake = wake == ESP_SLEEP_WAKEUP_EXT0 && boot_button_pressed();

    ESP_LOGI(TAG, "Boot: networks=%u refresh=%u reset=%d wake=%d boot_button=%d",
             cfg.network_count, cfg.refresh_min, reset, wake, boot_wake);

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
        storage_load_v2(&cfg);
        if (cfg.network_count == 0) storage_seed_current_sta(&cfg);
        esp_restart();
    }

    int network_idx = -1;
    int api_idx = -1;
    static dashboard_data_t data;   /* keep off the stack, like cfg above */
    memset(&data, 0, sizeof(data));

    /* On failure either retry in a foreground loop (bring-up — keeps
     * USB-CDC alive so we can reflash) or drop to deep sleep (battery-
     * friendly default for production). Toggle via Kconfig.
     *
     * When the retry loop is enabled, also bring up Improv so the web
     * flasher can read or rewrite the config without first holding BOOT to
     * force-enter the provisioning window. On ESP32-S3 this is the built-in
     * USB-Serial-JTAG CDC port; non-S3 builds fall back to UART0. The task
     * uses ~20 KiB of stack while alive — acceptable in the bring-up profile,
     * off entirely in production. */
    bool offline_shown = false;
#if CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
    bool improv_running = false;
    if (improv_start(NULL, 0) == ESP_OK) {
        improv_running = true;
        ESP_LOGI(TAG, "Improv ready on serial for config recovery");
    } else {
        ESP_LOGW(TAG, "improv_start failed; serial config unavailable");
    }
#endif
    for (;;) {
        err = wifi_roam_connect(&cfg, &network_idx);
        if (err == ESP_OK) {
            err = api_client_fetch_with_failover(&cfg, network_idx,
                                                 &data, &api_idx);
            wifi_net_stop();
            if (err == ESP_OK) break;
            ESP_LOGE(TAG, "API fetch failed");
        } else {
            ESP_LOGW(TAG, "No configured WiFi network available");
            wifi_net_stop();
        }

#if CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
        /* Refresh the offline screen only on the first failure of an
         * outage episode. The panel can't refresh faster than ~3 minutes
         * without ghosting, so silently retry in the background after
         * that. display_render() on the next success will repaint. */
        if (!offline_shown) {
            display_show_offline();
            offline_shown = true;
        }
        ESP_LOGW(TAG, "Offline, retrying in %ds",
                 CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S * 1000));
        continue;
#else
        display_show_offline();
        enter_deep_sleep(cfg.refresh_min);
        return;
#endif
    }

#if CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
    if (improv_running) improv_stop();
#endif
    display_render(&data);
    enter_deep_sleep(cfg.refresh_min);
}
