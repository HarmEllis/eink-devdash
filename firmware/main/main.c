#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_prov.h"
#include "wifi_roam.h"
#include "api_client.h"
#include "display.h"
#include "boot_button.h"
#include <string.h>

static const char *TAG = "main";
static const char *BUILD_MARKER =
    "diag-api-display-2026-05-21T21:25+02:00";
#define BOOT_WAKE_GPIO GPIO_NUM_0

static void enter_deep_sleep(uint8_t minutes)
{
    if (minutes < 3)  minutes = 3;
    if (minutes > 60) minutes = 60;
    uint64_t us = (uint64_t)minutes * 60ULL * 1000000ULL;
    ESP_LOGI(TAG, "Deep sleep %u min", minutes);

    ESP_ERROR_CHECK(rtc_gpio_init(BOOT_WAKE_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(BOOT_WAKE_GPIO,
                                          RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_set_direction_in_sleep(BOOT_WAKE_GPIO,
                                                   RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_WAKE_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_WAKE_GPIO));
    ESP_ERROR_CHECK(gpio_sleep_sel_dis(BOOT_WAKE_GPIO));

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(us));
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,
                                        ESP_PD_OPTION_ON));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_WAKE_GPIO, 0));
    esp_deep_sleep_start();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Firmware marker: %s", BUILD_MARKER);

#if CONFIG_DEVDASH_DEMO_MODE
    /* Demo build: render a plausible sample dashboard once and idle the
     * chip. No NVS, no WiFi, no API — purely a visual stand-in to show the
     * device to other people without needing network configuration. */
    static dashboard_data_t demo = {
        .schema_version = 1,
        .github_present = true,
        .github = { .issues = 7, .prs = 3, .dependabot = 2 },
        .claude = {
            .five_hour = { .used = 84, .limit = 200, .reset_in_seconds = 5400 },
            .weekly    = { .used = 1240, .limit = 5000, .reset_in_seconds = 205200 },
            .auth_error = false,
        },
        .codex = {
            .short_pct = 37,
            .long_pct = 27,
            .reached = false,
        },
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
    boot_button_init();

    ESP_ERROR_CHECK(wifi_net_init());
    /* cfg is 7+ KiB. Keep it in BSS instead of on the main task stack
     * (CONFIG_ESP_MAIN_TASK_STACK_SIZE is only 3584 bytes). */
    static dash_config_v2_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    storage_load_v2(&cfg);

    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    esp_reset_reason_t reset = esp_reset_reason();

    /* EXT0 wake from deep sleep fires when GPIO0 is held LOW.
     * Require the button to remain held for CONFIG_DEVDASH_BOOT_LONGPRESS_MS
     * before we treat the wake as a provisioning request. A short press
     * therefore acts as a free "force refresh now" trigger. */
    bool boot_wake = false;
    if (wake == ESP_SLEEP_WAKEUP_EXT0 && boot_button_is_pressed()) {
        if (boot_button_wait_longpress(CONFIG_DEVDASH_BOOT_LONGPRESS_MS)) {
            boot_button_wait_release();
            boot_wake = true;
        } else {
            ESP_LOGI(TAG, "Short BOOT wake — refresh cycle, no portal");
        }
    }

    /* Force-provisioning flag set by the in-app long-press monitor before
     * esp_restart(). Stored in RTC_NOINIT memory inside boot_button.c so it
     * survives the soft reset but zero-initialises on cold boot (BOARD_NOTES). */
    bool force_prov = boot_button_force_prov_consume();

    ESP_LOGI(TAG, "Boot: networks=%u refresh=%u reset=%d wake=%d boot_wake=%d force_prov=%d",
             cfg.network_count, cfg.refresh_min, reset, wake, boot_wake, force_prov);

    if (boot_wake || force_prov || cfg.network_count == 0) {
        char prov_ssid[32], prov_pop[16];
        wifi_net_get_prov_info(prov_ssid, sizeof(prov_ssid),
                               prov_pop, sizeof(prov_pop));
        display_show_qr(prov_ssid, prov_pop);

        /* config_window keeps the portal open against an already-configured
         * device (long-press recovery and EXT0 wake). provision_if_needed
         * is the fresh-flash path that exits early if creds already exist. */
        bool reconfigure = boot_wake || force_prov;
        err = reconfigure ? wifi_net_open_config_window()
                          : wifi_net_provision_if_needed();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning/config window err=%d", err);
            /* ESP_ERR_TIMEOUT = user did not finish in time; the portal was
             * up. Anything else (FAIL etc.) means the SoftAP itself did not
             * start, so paint the V4 S1 red error variant. */
            if (err == ESP_ERR_TIMEOUT) {
                display_show_offline(DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT,
                                     &cfg, -1, NULL, NULL);
            }
            else                        display_show_setup_failed();
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
    wifi_unreachable_diag_t wifi_diag = {0};
    api_unreachable_diag_t api_diag = {0};
    static dashboard_data_t data;   /* keep off the stack, like cfg above */
    memset(&data, 0, sizeof(data));

    /* From here on we are in the normal connect/fetch/render path. Start
     * the BOOT long-press monitor so a 5s hold can force the captive
     * portal at any time — including while stuck in the offline retry
     * loop below. The monitor writes the RTC force_prov flag and calls
     * esp_restart(); on next boot we land in the portal branch above. */
    boot_button_monitor_start();

    /* On failure either retry in a foreground loop (bring-up — keeps
     * USB-CDC alive so we can reflash) or drop to deep sleep (battery-
     * friendly default for production). Toggle via Kconfig. */
    bool offline_shown = false;
    bool wake_refresh = (wake == ESP_SLEEP_WAKEUP_TIMER ||
                         wake == ESP_SLEEP_WAKEUP_EXT0);
    bool prefer_last_success_api = wake_refresh;
    if (!wake_refresh) {
        display_show_connecting(false, &cfg);
    }
    for (;;) {
        display_offline_reason_t offline_reason = DISPLAY_OFFLINE_REASON_WIFI;
        memset(&wifi_diag, 0, sizeof(wifi_diag));
        memset(&api_diag, 0, sizeof(api_diag));
        err = wifi_roam_connect(&cfg, &network_idx, &wifi_diag);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected; selected network index=%d", network_idx);
            ESP_LOGI(TAG, "Fetching dashboard API (%s)",
                     prefer_last_success_api ? "prefer last successful slot"
                                             : "ordered from first slot");
            err = api_client_fetch_with_failover(&cfg, network_idx,
                                                 prefer_last_success_api,
                                                 &data, &api_idx,
                                                 &api_diag);
            ESP_LOGI(TAG, "Dashboard API fetch result: %s api_idx=%d",
                     esp_err_to_name(err), api_idx);
            wifi_net_stop();
            if (err == ESP_OK) break;
            offline_reason = DISPLAY_OFFLINE_REASON_API;
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
            display_show_offline(offline_reason, &cfg, network_idx,
                                 &wifi_diag, &api_diag);
            offline_shown = true;
        }
        ESP_LOGW(TAG, "Offline, retrying in %ds",
                 CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S * 1000));
        continue;
#else
        display_show_offline(offline_reason, &cfg, network_idx,
                             &wifi_diag, &api_diag);
        enter_deep_sleep(cfg.refresh_min);
        return;
#endif
    }

    display_set_connection_slots(&cfg, network_idx, api_idx);
    display_render(&data);
    enter_deep_sleep(cfg.refresh_min);
}
