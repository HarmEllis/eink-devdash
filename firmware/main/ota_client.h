#pragma once
#include "esp_err.h"
#include "storage.h"

/*
 * Run a single OTA cycle, if appropriate. Called from app_main after the
 * dashboard API has been fetched, but before the dashboard is rendered. A
 * real update paints one static OTA poster, then starts the HTTPS download
 * and flash write; rendering the dashboard first would cause two full
 * e-paper refreshes in one wake cycle.
 *
 * Behaviour:
 *  - Fetches `<base_url>/ota/manifest` over plain HTTP with the existing
 *    Bearer auth for the picked network/api slot.
 *  - If the API reports otaEnabled=false, returns ESP_OK without action.
 *  - If latestVersion equals the running esp_app_get_description()->version,
 *    returns ESP_OK without action.
 *  - Otherwise runs esp_https_ota against the manifest's downloadUrl using
 *    the bundled Mozilla cert authority. On success, reboots (does not
 *    return).
 *  - On failure, bumps the RTC throttle counter so the next
 *    DEVDASH_OTA_THROTTLE_CYCLES wake cycles skip OTA. The throttle is
 *    zeroed on cold boot (RTC_NOINIT_ATTR) and on a successful update.
 *
 * Returns ESP_OK when no update was needed or the device is about to
 * reboot. Returns the underlying esp_err_t when an attempt failed.
 */
esp_err_t ota_client_maybe_update(const dash_config_v2_t *cfg,
                                  int network_idx,
                                  int api_idx);

/*
 * Called by main.c once the running image has successfully fetched and
 * rendered a dashboard at least once. If the image is currently
 * PENDING_VERIFY, this commits it so the bootloader stops rolling back.
 * Safe to call on every boot; no-op when the image is already VALID.
 */
void ota_client_mark_image_valid(void);
