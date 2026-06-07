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
 *  - Fetches `<base_url>/ota/manifest` over HTTP or HTTPS with the existing
 *    Bearer auth for the picked network/api slot. A relay profile resolves
 *    this to `/d/<uuid>/ota/manifest`, served from the manifest the API
 *    publishes over its outbound WebSocket; a 404 (older relay / no manifest
 *    / past TTL) is a graceful no-op, not a throttled failure.
 *  - If the API reports otaEnabled=false, returns ESP_OK without action.
 *  - Upgrade-only: installs only when latestVersion is strictly newer than the
 *    running esp_app_get_description()->version (numeric vMAJOR.MINOR.PATCH
 *    compare). Equal, older, or malformed versions return ESP_OK without
 *    action (see ota_version.h, ota_version_is_newer).
 *  - Trust anchor: the manifest's downloadUrl must be byte-for-byte the
 *    canonical GitHub Releases URL for latestVersion (see
 *    ota_download_url_is_canonical); a non-canonical URL is refused without
 *    action so a leaked publish key cannot install a foreign binary.
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
