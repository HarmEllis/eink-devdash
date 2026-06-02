#pragma once
#include "api_client.h"
#include "storage.h"
#include "unreachable_diag.h"
#include "eink_weact29.h"

/* Provisioning / recovery surfaces and the normal post-provisioning boot
   share a few helpers (show_connecting, show_refreshing, the boot/wait
   full-refreshes). The bootstrap path must route them through the
   panel-agnostic SAFE_BW helper, while the normal-boot path must keep
   variant-aware rendering so red residue is cleared on BWR. The caller
   picks via this context tag. */
typedef enum {
    DISPLAY_CTX_PROVISIONING_RECOVERY = 0,
    DISPLAY_CTX_NORMAL_BOOT           = 1,
} display_context_t;

/* Mark which physical panel is attached. main.c calls this once per boot,
   after storage_load_v2 returns a real persisted blob. Until called, the
   display layer routes through the BW-safe bootstrap path regardless of
   the defaulted cfg.panel_variant value. */
void display_set_panel_variant(eink_panel_variant_t v);

/* Store the configured refresh interval (clamped 3..60) for the
   render-count cap. main.c calls this on both branches of
   storage_load_v2 — its value gates the wall-clock-proxy that forces a
   periodic full BW refresh to clear ghost accumulation. */
void display_set_refresh_min(uint8_t refresh_min);

/* Set a one-shot flag so the next render runs a full refresh regardless
   of the partial-region plan. Used by post-restart variant toggles. */
void display_force_full_refresh_next(void);

void display_render(const dashboard_data_t *data);
void display_set_connection_slots(const dash_config_v2_t *cfg,
                                  int network_idx,
                                  int active_api_idx);
void display_show_connecting(display_context_t ctx, bool compact,
                             const dash_config_v2_t *cfg);
void display_show_refreshing(display_context_t ctx, bool compact);
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
