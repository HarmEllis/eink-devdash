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
#include "ota_client.h"
#include "timekeep.h"
#include "runtime_policy.h"
#include <string.h>

static const char *TAG = "main";
static const char *BUILD_MARKER =
    "diag-api-display-2026-05-21T21:25+02:00";
#define BOOT_WAKE_GPIO GPIO_NUM_0

/* RTC-memory flag: true once the sleeping footer has been painted for the
 * current quiet-hours window. Survives the chunked deep-sleep re-evaluations
 * (so we don't repaint the e-paper every chunk) and zero-inits on cold boot.
 * Reset to false on a normal dashboard render so the next window repaints. */
static RTC_DATA_ATTR bool s_quiet_footer_shown = false;

typedef struct {
    uint32_t magic;
    uint32_t attempt;
    uint8_t reason;
} offline_episode_t;

#define OFFLINE_EPISODE_MAGIC 0x0FF11E01u

static RTC_DATA_ATTR offline_episode_t s_offline_episode;

static uint32_t offline_attempt_next(display_offline_reason_t reason)
{
    if (s_offline_episode.magic != OFFLINE_EPISODE_MAGIC ||
        s_offline_episode.reason != (uint8_t)reason) {
        s_offline_episode.magic = OFFLINE_EPISODE_MAGIC;
        s_offline_episode.attempt = 1;
        s_offline_episode.reason = (uint8_t)reason;
    } else if (s_offline_episode.attempt < UINT32_MAX) {
        s_offline_episode.attempt++;
    }
    return s_offline_episode.attempt;
}

static void offline_episode_clear(void)
{
    memset(&s_offline_episode, 0, sizeof(s_offline_episode));
}

static void enter_deep_sleep_seconds(uint32_t seconds)
{
    /* Floor at 60 s so a window boundary that is < 1 min away cannot spin the
     * chip in a tight wake loop. */
    if (seconds < 60) seconds = 60;
    uint64_t us = (uint64_t)seconds * 1000000ULL;
    ESP_LOGI(TAG, "Deep sleep %u s", (unsigned)seconds);

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

static void enter_deep_sleep(uint8_t minutes)
{
    if (minutes < DASH_REFRESH_MIN_BW_TWO_PARTIALS) {
        minutes = DASH_REFRESH_MIN_BW_TWO_PARTIALS;
    }
    if (minutes > DASH_REFRESH_MAX) minutes = DASH_REFRESH_MAX;
    enter_deep_sleep_seconds((uint32_t)minutes * 60u);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Firmware marker: %s", BUILD_MARKER);

#if CONFIG_DEVDASH_DEMO_MODE
    /* Demo build: render a plausible sample dashboard once and idle the
     * chip. No NVS, no WiFi, no API — purely a visual stand-in to show the
     * device to other people without needing network configuration. */
    static dashboard_data_t demo = {
        .schema_version = 2,
        .github_present = true,
        .github = {
            .issues = 12,
            .prs = 4,
            .dependabot = 0,
            .notifications = 1,
            .notifications_present = true,
        },
        .claude = {
            .five_hour = { .used = 18, .limit = 200, .reset_in_seconds = 8200 },
            .weekly    = { .used = 4100, .limit = 10000, .reset_in_seconds = 304800 },
            .auth_error = false,
        },
        .codex = {
            .short_pct = 32,
            .long_pct = 38,
            .reached = false,
            .short_reset_in_seconds = 3600,
            .long_reset_in_seconds = 313200,
        },
        .updated_at = "14:38",
        .stale = false,
        .offline = false,
    };
    ESP_LOGI(TAG, "DEMO mode: rendering static sample dashboard");
    /* Demo builds skip NVS, so storage_load_v2() never runs. Wire the
       display setters by hand so red still renders on a BWR demo unit
       (default) or collapses to BW on a BW demo unit. */
    display_set_refresh_min(CONFIG_DEVDASH_REFRESH_MIN);
    display_set_max_partials(DASH_MAX_PARTIALS_DEFAULT);
    display_set_panel_variant(
        (eink_panel_variant_t)CONFIG_DEVDASH_DEMO_PANEL_VARIANT);
    display_render(&demo);
    for (;;) vTaskDelay(portMAX_DELAY);
#endif

    /* If a device-side factory reset was armed from the setup-screen BOOT
     * gesture, erase the whole NVS partition before init (nvs_flash_erase
     * requires NVS to be uninitialised). Lets the user fully reset without the
     * web flasher. The flag lives in RTC memory and survives the esp_restart()
     * the gesture issues. */
    /* Gate the DESTRUCTIVE erase on a software-reset reason as well as the RTC
     * latch. The latch lives in RTC_NOINIT memory (it must survive the gesture's
     * esp_restart() — RTC_DATA_ATTR would be reinitialised on a non-deep-sleep
     * boot, per esp_attr.h, and silently lose it). RTC_NOINIT is undefined after
     * a cold/power-on boot, so a stale word equal to the magic could otherwise
     * trigger a no-gesture wipe. The gesture always reaches here via esp_restart
     * (ESP_RST_SW); a power-on (ESP_RST_POWERON) or deep-sleep wake therefore
     * never honours the latch, making the worst case fail safe. */
    if (boot_button_factory_reset_pending() &&
        esp_reset_reason() == ESP_RST_SW) {
        ESP_LOGW(TAG, "Factory reset armed: erasing entire NVS partition");
        esp_err_t ferr = nvs_flash_erase();
        if (ferr == ESP_OK) {
            boot_button_factory_reset_clear();
            ESP_LOGW(TAG, "Factory reset: NVS erased");
        } else {
            /* Fail closed: never fall through into normal operation with the
             * secrets the user asked to wipe. Keep the RTC flag set and restart
             * after a short delay so the erase is retried, rather than booting
             * the old config. */
            ESP_LOGE(TAG, "Factory reset erase failed: %s — restarting to retry",
                     esp_err_to_name(ferr));
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    }

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
    bool cfg_loaded = storage_load_v2(&cfg);
    /* Overlay the RTC-backed reconnect hints (last successful network/API).
       These live in RTC memory, not the NVS blob, to avoid per-wake blob
       rewrites; apply them onto the freshly loaded cfg before first use. */
    storage_apply_last_success(&cfg);

    /* Always seed the refresh interval — even on a defaulted cfg — so the
       display layer's 24h forced-full cap uses the right cadence. */
    display_set_refresh_min(cfg.refresh_min);
    /* Seed the BW per-region partial cap from the (possibly defaulted) cfg. */
    display_set_max_partials(cfg.max_partials);
    /* Resolve the panel variant for this boot (Gate 0.B fallback): it is always
       known by the first draw, so recovery / provisioning surfaces render
       through the correct variant path (FULL_COLOR clears red on BWR) instead
       of the dormant panel-agnostic SAFE_BW path that failed on a
       red-preconditioned BWR panel.
         - a real saved v3 config wins (portal choice / migrated value);
         - otherwise the build-stamped SKU default
           (CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT, via storage_default_panel_variant). */
    eink_panel_variant_t variant;
    const char *variant_src;
    if (cfg_loaded) {
        variant = (eink_panel_variant_t)cfg.panel_variant;
        variant_src = "cfg";
    } else {
        variant = storage_default_panel_variant();
        variant_src = "default";
    }
    display_set_panel_variant(variant);
    ESP_LOGI(TAG, "Panel variant=%s (source=%s)",
             variant == EINK_PANEL_WEACT_29_BW ? "BW" : "BWR", variant_src);

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

    /* The provisioning flag is a session latch, not a one-shot. It remains set
     * across software/USB resets until the portal saves successfully or the
     * provisioning window returns with a controlled error. */
    bool force_prov = boot_button_force_prov_active();

    ESP_LOGI(TAG, "Boot: networks=%u refresh=%u reset=%d wake=%d boot_wake=%d force_prov=%d",
             cfg.network_count, cfg.refresh_min, reset, wake, boot_wake, force_prov);

    if (boot_wake || force_prov || cfg.network_count == 0) {
        boot_button_force_prov_mark();
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
            boot_button_force_prov_clear();
            ESP_LOGE(TAG, "Provisioning/config window err=%d", err);
            /* ESP_ERR_TIMEOUT = user did not finish in time; the portal was
             * up. Anything else (FAIL etc.) means the SoftAP itself did not
             * start, so paint the V4 S1 red error variant. */
            if (err == ESP_ERR_TIMEOUT) {
                display_show_offline(DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT,
                                     &cfg, -1, NULL, NULL, 0);
            }
            else                        display_show_setup_failed();
            wifi_net_stop();
            enter_deep_sleep(cfg.refresh_min);
            return;
        }
        storage_load_v2(&cfg);
        esp_restart();
    }

    /* Quiet hours: on a plain timer refresh (NOT cold boot, NOT a short-press
     * EXT0 force-refresh, NOT provisioning) skip the whole WiFi + API + e-paper
     * cycle when the network we are parked on is inside its configured sleep
     * window. The window is evaluated against the RTC clock, which we set from
     * the API after each fetch and which survives timer wakes but not cold
     * boot — so an unset clock simply falls through to a normal refresh that
     * re-acquires the time. We re-evaluate on every wake (chunked sleep) so the
     * device leaves the window within at most one chunk. */
    if (wake == ESP_SLEEP_WAKEUP_TIMER) {
        int qidx = cfg.last_success_network_idx;
        int now_min = 0;
        if (qidx >= 0 && qidx < cfg.network_count &&
            cfg.networks[qidx].enabled && cfg.networks[qidx].ssid[0] != '\0' &&
            cfg.networks[qidx].quiet_enabled &&
            timekeep_now_minute_of_day(&now_min) &&
            timekeep_minute_in_window(now_min, cfg.networks[qidx].quiet_start_min,
                                      cfg.networks[qidx].quiet_end_min)) {
            int end_min = cfg.networks[qidx].quiet_end_min;
            int until = timekeep_minutes_until(now_min, end_min);
            if (!s_quiet_footer_shown) {
                char wake_hhmm[9];
                snprintf(wake_hhmm, sizeof(wake_hhmm), "%02d:%02d",
                         end_min / 60, end_min % 60);
                display_show_sleeping(wake_hhmm);
                s_quiet_footer_shown = true;
            }
            /* Cap each nap at 60 min so RTC drift cannot overshoot the window
             * end by more than a chunk; the floor lives in
             * enter_deep_sleep_seconds. */
            uint32_t chunk_min = (until > 60) ? 60u : (uint32_t)until;
            ESP_LOGI(TAG, "Quiet hours net=%d now=%02d:%02d wake=%02d:%02d sleep=%umin",
                     qidx, now_min / 60, now_min % 60, end_min / 60, end_min % 60,
                     (unsigned)chunk_min);
            wifi_net_stop();
            enter_deep_sleep_seconds(chunk_min * 60u);
            return;
        }
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
#if CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
    bool offline_displayed = false;
    display_offline_reason_t displayed_offline_reason =
        DISPLAY_OFFLINE_REASON_WIFI;
    TickType_t next_offline_display_at = 0;
#endif
    bool wake_refresh = (wake == ESP_SLEEP_WAKEUP_TIMER ||
                         wake == ESP_SLEEP_WAKEUP_EXT0);
    bool prefer_last_success_api = wake_refresh;
    if (!wake_refresh) {
        display_show_connecting(DISPLAY_CTX_NORMAL_BOOT, false, &cfg);
    }
#if !CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
    /* Counts the whole connect+fetch cycles tried this wake (production
     * deep-sleep path only); the bring-up branch retries forever instead.
     * wake_started marks when retrying began so the cutoff below can stop
     * starting new cycles (it does not interrupt a cycle already running). */
    uint32_t wake_attempt = 0;
    TickType_t wake_started = xTaskGetTickCount();
#endif
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
            if (err == ESP_OK) break;
            wifi_net_stop();
            offline_reason = DISPLAY_OFFLINE_REASON_API;
            ESP_LOGE(TAG, "API fetch failed");
        } else {
            ESP_LOGW(TAG, "No configured WiFi network available");
            wifi_net_stop();
        }

#if CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE
        /* Count every connection cycle, but update the panel no faster than
         * the configured dashboard interval. This keeps the bring-up retry
         * loop from bypassing the one-minute/two-partial safety contract. */
        uint32_t attempt = offline_attempt_next(offline_reason);
        TickType_t now = xTaskGetTickCount();
        bool reason_changed =
            offline_displayed && displayed_offline_reason != offline_reason;
        bool display_due =
            !offline_displayed || reason_changed ||
            (int32_t)(now - next_offline_display_at) >= 0;
        if (display_due) {
            display_show_offline(offline_reason, &cfg, network_idx,
                                 &wifi_diag, &api_diag, attempt);
            offline_displayed = true;
            displayed_offline_reason = offline_reason;
            next_offline_display_at =
                now + pdMS_TO_TICKS((uint32_t)cfg.refresh_min * 60u * 1000u);
        }
        ESP_LOGW(TAG, "Offline, retrying in %ds",
                 CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DEVDASH_OFFLINE_RETRY_INTERVAL_S * 1000));
        continue;
#else
        /* Absorb a transient scan miss, slow DHCP or relay cold-start by
         * retrying the whole connect+fetch cycle a bounded number of times
         * before giving up. WiFi is already stopped on both failure paths
         * above, and each retry gets a fresh API fetch-cycle budget. An
         * unambiguous permanent API error (no profile / missing token) is not
         * retried; everything else (including all WiFi failures, which are
         * ambiguous to classify) is retried until the attempt count or the
         * elapsed-time cutoff is reached. The cutoff is evaluated between
         * cycles: it stops a new cycle from starting once exceeded, but does
         * not clamp the per-profile/fetch waits inside a cycle already under
         * way, so the last started cycle can still run to its own timeouts. */
        bool permanent = (offline_reason == DISPLAY_OFFLINE_REASON_API)
                             ? api_diag.permanent
                             : false;
        bool cutoff_passed =
            (int32_t)(xTaskGetTickCount() - wake_started) >=
            (int32_t)pdMS_TO_TICKS(DASH_WAKE_RETRY_CUTOFF_MS);
        wake_attempt++;
        if (!permanent && wake_attempt < DASH_WAKE_RETRY_MAX_ATTEMPTS &&
            !cutoff_passed) {
            ESP_LOGW(TAG,
                     "Connect/fetch cycle %u/%u failed (%s); retrying in %d ms",
                     (unsigned)wake_attempt,
                     (unsigned)DASH_WAKE_RETRY_MAX_ATTEMPTS,
                     offline_reason == DISPLAY_OFFLINE_REASON_API ? "API"
                                                                  : "WiFi",
                     DASH_WAKE_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(DASH_WAKE_RETRY_DELAY_MS));
            continue;
        }
        if (permanent) {
            ESP_LOGW(TAG, "Offline cause looks permanent (%s); sleeping without retry",
                     offline_reason == DISPLAY_OFFLINE_REASON_API ? "API"
                                                                  : "WiFi");
        } else if (cutoff_passed) {
            ESP_LOGW(TAG, "Per-wake retry cutoff reached after %u cycle(s); sleeping",
                     (unsigned)wake_attempt);
        }
        uint32_t attempt = offline_attempt_next(offline_reason);
        display_show_offline(offline_reason, &cfg, network_idx,
                             &wifi_diag, &api_diag, attempt);
        enter_deep_sleep(cfg.refresh_min);
        return;
#endif
    }

    /* Set the RTC wall clock from the API's local timestamp so the next timer
     * wake can evaluate the quiet-hours window without turning on WiFi. */
    if (clock_should_apply(data.updated_at_iso, data.stale)) {
        timekeep_set_from_iso(data.updated_at_iso);
    }
    /* A normal refresh repaints the dashboard, so the sleeping footer (if any)
     * is gone — let the next quiet-window entry repaint it. */
    s_quiet_footer_shown = false;
    offline_episode_clear();

    display_set_connection_slots(&cfg, network_idx, api_idx);

    /* Check for OTA before the dashboard render. A real update writes one
     * static OTA poster and then starts flash erase/write; rendering the
     * dashboard first would force two full e-paper refreshes in one wake
     * cycle. */
    err = ota_client_maybe_update(&cfg, network_idx, api_idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA check/update failed: %s", esp_err_to_name(err));
    }

    wifi_net_stop();
    display_render(&data);

    /* A successful fetch + render is the rollback-validation gate for an
     * OTA install. No-op when the running image is already VALID, so this
     * is safe to call unconditionally. */
    ota_client_mark_image_valid();

    enter_deep_sleep(cfg.refresh_min);
}
