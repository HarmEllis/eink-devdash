#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "eink_weact29.h"

#define NVS_NAMESPACE "devdash"
#define NVS_SCHEMA_VERSION 5

#define DASH_CFG_V2_VERSION 5
#define MAX_WIFI_NETWORKS 5
#define MAX_APIS_PER_NETWORK 5
#define DASH_SSID_MAX 32
#define DASH_WIFI_PASSWORD_MAX 64
#define DASH_API_URL_MAX 192
#define DASH_DEVICE_TOKEN_MAX 64

/* BW per-region partial-refresh cap (max_partials): how many partial refreshes
   a BW region may take before it is forced to do a full refresh. Portal-editable;
   inert on BWR (which always full-refreshes). */
#define DASH_MAX_PARTIALS_MIN     1
#define DASH_MAX_PARTIALS_MAX     100
#define DASH_MAX_PARTIALS_DEFAULT 5

/* Per-network "quiet hours" (added in v5). Window endpoints are minutes since
   local midnight, so the valid range is [0, 1439]. A window with
   start == end is treated as disabled (see storage_cfg_v2_normalize). */
#define DASH_QUIET_MIN_OF_DAY_MAX 1439

/*
 * Hard ceiling for the cfg_v2 NVS blob. NVS supports blobs up to ~97% of
 * the partition size, but a single fat blob means losing every network on
 * any corrupted save. If the static assertions below ever fire, switch to
 * per-network blobs instead of widening this cap.
 */
#define DASH_CFG_V2_MAX_BYTES 8192

typedef struct {
    uint32_t id;
    bool enabled;
    char api_url[DASH_API_URL_MAX];
    char device_token[DASH_DEVICE_TOKEN_MAX];
} dash_api_profile_t;

typedef struct {
    uint32_t id;
    bool enabled;
    char ssid[DASH_SSID_MAX + 1];
    char password[DASH_WIFI_PASSWORD_MAX + 1];
    uint8_t api_count;
    dash_api_profile_t apis[MAX_APIS_PER_NETWORK];
} dash_wifi_profile_t;

typedef struct {
    uint16_t version;
    uint8_t max_wifi_networks;
    uint8_t max_apis_per_network;
    uint8_t refresh_min;
    uint8_t network_count;
    int8_t last_success_network_idx;
    int8_t last_success_api_idx;
    uint32_t write_counter;
    uint32_t crc32;
    dash_wifi_profile_t networks[MAX_WIFI_NETWORKS];
    /* Added in v3. Storage layout is layered so v2 blobs migrate cleanly:
       fields above are byte-identical with the v2 struct; panel_variant
       only exists in v3. Stored as raw uint8_t and cast to
       eink_panel_variant_t on read after cfg_v2_is_valid() accepts the
       value. */
    uint8_t panel_variant;
    /* Added in v4. Like panel_variant, this is appended as a trailing field; it
       lands in the existing 4-byte-alignment tail padding after panel_variant,
       so sizeof(v4) == sizeof(v3) and the load path must distinguish v3 from v4
       by the `version` field, NOT by blob length. BW per-region partial cap,
       clamped to [DASH_MAX_PARTIALS_MIN, DASH_MAX_PARTIALS_MAX]. */
    uint8_t max_partials;
    /* Added in v5. Per-network "quiet hours": a local-time window during which
       the device skips the WiFi + API + e-paper refresh cycle and just deep-
       sleeps. Stored as TRAILING parallel arrays indexed by network slot (NOT
       inside dash_wifi_profile_t) so the networks[] offset stays byte-identical
       with v2/v3/v4 — the same migration invariant the static assertions pin
       down. Unlike panel_variant/max_partials these do NOT fit in tail padding,
       so sizeof(v5) > sizeof(v4); the load path distinguishes v5 from v3/v4 by
       blob length. quiet_enabled[i] is 0/1; quiet_start_min[i]/quiet_end_min[i]
       are minutes since local midnight in [0, DASH_QUIET_MIN_OF_DAY_MAX]. A
       window with start == end is treated as disabled. */
    uint8_t  quiet_enabled[MAX_WIFI_NETWORKS];
    uint16_t quiet_start_min[MAX_WIFI_NETWORKS];
    uint16_t quiet_end_min[MAX_WIFI_NETWORKS];
} dash_config_v2_t;

void storage_init(void);
/* Returns true when a v2, v3, or v4 blob was successfully loaded from NVS,
   false when the caller is looking at storage_cfg_v2_defaults() (either
   no blob exists, or the stored blob failed length/CRC/value validation).
   Existing callers may safely ignore the return value; passing it on is
   how callers distinguish a real persisted config from defaults. */
bool storage_load_v2(dash_config_v2_t *cfg);
esp_err_t storage_save_v2(dash_config_v2_t *cfg);
esp_err_t storage_seed_current_sta(dash_config_v2_t *cfg);
void storage_erase(void);

void storage_cfg_v2_defaults(dash_config_v2_t *cfg);
void storage_cfg_v2_normalize(dash_config_v2_t *cfg);

/* last_success_network_idx/api_idx are a reconnect optimization: they let the
   next wake try the network/API slot that last worked first, instead of
   re-scanning from 0. They need NOT survive a cold boot (a fresh device just
   scans), so they live in RTC memory — NOT the NVS blob. Persisting them in
   NVS used to rewrite the whole ~7 KB cfg_v2 blob on every successful wake
   (wifi_roam + api_client), fragmenting the shared 24 KB partition until saves
   failed with ESP_ERR_NVS_NOT_ENOUGH_SPACE. RTC memory survives deep sleep and
   esp_restart, costs no flash wear, and resets on cold boot — exactly the
   lifetime these hints want. See AGENTS.md "NVS writes and RTC memory".

   storage_note_last_success: call after a successful connect/fetch to record
   the working slot (RTC write only, no flash).
   storage_apply_last_success: call ONCE right after storage_load_v2 to overlay
   the RTC hints onto the freshly loaded cfg, pair-clamped to the current
   network/API layout (both reset to -1 if either is out of range). */
void storage_note_last_success(int8_t net_idx, int8_t api_idx);
void storage_apply_last_success(dash_config_v2_t *cfg);

/* Build-stamped SKU default panel variant (CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT).
   Seeds the defaults / migration when no real saved variant exists; a genuine
   saved v3 panel_variant always wins. See Gate 0.B in BOARD_NOTES. */
eink_panel_variant_t storage_default_panel_variant(void);
bool storage_validate_api_url(const char *url);
uint32_t storage_next_profile_id(const dash_config_v2_t *cfg);
void storage_mask_token(const char *token, char *out, size_t out_sz);

/* AP password used for the SoftAP provisioning portal. On first call after
 * a fresh flash (or after storage_erase), a 12-character mixed-case
 * alphanumeric string is generated from esp_fill_random and persisted in
 * NVS under key "ap_pwd". All subsequent calls return the persisted value.
 * Writes `out` with a NUL-terminated 12-char string (out_sz must be >= 13).
 * Returns ESP_OK on success. */
esp_err_t storage_get_or_init_ap_password(char *out, size_t out_sz);
