#pragma once
#include "api_client.h"
#include "storage.h"
#include "unreachable_diag.h"
#include "eink_weact29.h"

/* Provisioning / recovery surfaces and the normal post-provisioning boot
   share a few helpers (show_connecting, show_refreshing, the boot/wait
   full-refreshes). The panel variant is now resolved before the first draw
   (persisted blob or build-stamped SKU default), so
   every surface renders through the variant-aware full refresh — FULL_COLOR
   on BWR (which clears red residue) and BW_FULL on BW. This tag no longer
   selects SAFE_BW vs variant-aware; it only marks whether a saved dashboard
   frame may be present, so the normal-boot path is allowed to overlay a
   compact status line while the provisioning/recovery path always draws a
   full poster. */
typedef enum {
    DISPLAY_CTX_PROVISIONING_RECOVERY = 0,
    DISPLAY_CTX_NORMAL_BOOT           = 1,
} display_context_t;

/* Mark which physical panel is attached. main.c calls this once per boot,
   before the first surface is drawn, with the variant resolved from the
   persisted blob or the build-stamped SKU default. This gates the
   compact-status overlay (which needs a known variant) and ensures recovery
   surfaces use the correct variant-aware full refresh. */
void display_set_panel_variant(eink_panel_variant_t v);

/* Store the configured refresh interval (clamped 1..60) for the
   render-count cap. main.c calls this on both branches of
   storage_load_v2 — its value gates the wall-clock-proxy that forces a
   periodic full BW refresh to clear ghost accumulation. */
void display_set_refresh_min(uint8_t refresh_min);

/* Store the BW per-region partial cap (clamped to [DASH_MAX_PARTIALS_MIN,
   DASH_MAX_PARTIALS_MAX]): how many partial refreshes a BW region may take
   before it is forced to a full refresh. main.c calls this on both branches of
   storage_load_v2. Inert on BWR (which always full-refreshes). Independent of
   the 24h render-count cap, which can still force a full refresh on its own. */
void display_set_max_partials(uint8_t max_partials);

/* Set a one-shot flag so the next render runs a full refresh regardless
   of the partial-region plan. Used by post-restart variant toggles. */
void display_force_full_refresh_next(void);

void display_render(const dashboard_data_t *data);

/* Overlay the quiet-hours "sleeping" state on the last shown dashboard: a moon
 * + last-sync time in the header and a black footer bar
 * "[moon] SLEEPING [dot] WAKES HH:MM". One full refresh; the next dashboard
 * render forces a full refresh to clear the footer. */
void display_show_sleeping(const char *wake_hhmm);
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
