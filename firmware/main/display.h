#pragma once
#include "api_client.h"
#include "storage.h"
#include "unreachable_diag.h"

void display_render(const dashboard_data_t *data);
void display_set_connection_slots(const dash_config_v2_t *cfg,
                                  int network_idx,
                                  int active_api_idx);
void display_show_connecting(bool compact, const dash_config_v2_t *cfg);
void display_show_refreshing(bool compact);
/* Render the V4 S1 provisioning prompt with the per-device SoftAP SSID and
 * AP password. The QR is generated from wifi_net_get_wifi_qr at render time.
 * Pass NULL for either to fall back to a placeholder. */
void display_show_qr(const char *ssid, const char *pop);
/* V4 S1 error variant — drawn when the SoftAP fails to start. */
void display_show_setup_failed(void);

typedef enum {
    DISPLAY_OFFLINE_REASON_API,
    DISPLAY_OFFLINE_REASON_WIFI,
    DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT,
} display_offline_reason_t;

void display_show_offline(display_offline_reason_t reason,
                          const dash_config_v2_t *cfg,
                          int network_idx,
                          const wifi_unreachable_diag_t *wifi_diag,
                          const api_unreachable_diag_t *api_diag);

/* Static OTA install poster. This is intentionally a single full-refresh
 * frame shown before flash erase/write starts. The display layer must not
 * animate progress during OTA because the panel and flash operations are
 * both long-running. OTA draws no progress/success/failure frames, so this
 * is the only e-paper refresh in the update path before reboot. */
void display_show_ota_update(const char *from_version,
                             const char *to_version,
                             const char *slot_name,
                             const char *slot_label);
