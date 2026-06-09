#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "eink_weact29.h"

#define NVS_NAMESPACE "devdash"
#define NVS_SCHEMA_VERSION 6

#define DASH_CFG_V2_VERSION 6
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
 * Since config version 6 the persisted layout is PER-NETWORK blobs: a small
 * `cfg_meta` header blob plus one `cfg_net{i}{a|b}` blob per saved WiFi
 * network (profile + its APIs + its quiet hours). storage_save_v2 writes
 * changed blobs ONE AT A TIME — each into the slot's other bank key — and
 * commits the meta blob last, which switches the whole save live
 * atomically. Each changed slot adds one ~1.5 KB alternate blob instead of
 * rewriting a monolithic ~7.2 KB config blob; the preflight reserves the
 * combined cost of all changed slots before writing. This fixes
 * ESP_ERR_NVS_NOT_ENOUGH_SPACE on portal re-saves, and a save that fails
 * partway leaves the previous configuration fully intact. A corrupted blob
 * now costs one network, not all of them.
 *
 * Free-space model (enforced by a static assert in storage.c and a runtime
 * preflight guard in storage_save_v2): a save holds old+new copies of every
 * CHANGED network blob until the meta commit switches the new generation
 * live, so the absolute worst case is every slot double-banked plus old+new
 * meta coexistence. That peak, plus the misc keys (ap_pwd, wifi_cc), must
 * fit the 24 KB `nvs` partition (5 usable data pages of 126 × 32 B entries
 * after the reserved GC page) — the static assert pins it. The runtime
 * guard computes the exact entry cost of the pending save (changed blobs +
 * meta) and compares it against nvs_get_stats() available_entries AFTER
 * orphan cleanup, failing early with a clear error instead of starting a
 * doomed sequence. If the assert fires after a caps/size change, shrink the
 * caps — do NOT move or grow the partition (partition-table changes do not
 * propagate via OTA).
 */
#define DASH_NET_BLOB_MAX_BYTES 1536
/* Worst-case NVS entry cost of an N-byte blob: ceil(N/32) data entries plus
   blob-index + chunk overhead. */
#define DASH_NVS_BLOB_ENTRIES(n) (((n) + 31u) / 32u + 3u)

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
    /* Per-network "quiet hours": a local-time window during which the device
       skips the WiFi + API + e-paper refresh cycle and just deep-sleeps.
       quiet_enabled is 0/1; quiet_start_min/quiet_end_min are minutes since
       local midnight in [0, DASH_QUIET_MIN_OF_DAY_MAX]. A window with
       start == end is treated as disabled. (v5 stored these as trailing
       parallel arrays for single-blob layout compatibility; since v6 each
       network blob owns its quiet fields.) */
    uint8_t quiet_enabled;
    uint16_t quiet_start_min;
    uint16_t quiet_end_min;
    dash_api_profile_t apis[MAX_APIS_PER_NETWORK];
} dash_wifi_profile_t;

/* In-RAM aggregate config. Since v6 this is NOT the persisted layout — see
   the per-network blob note above; the exact v2-v5 single-blob layouts live
   as private legacy structs in storage.c for migration only. write_counter
   mirrors the meta blob; per-blob CRCs are storage-internal. */
typedef struct {
    uint16_t version;
    uint8_t max_wifi_networks;
    uint8_t max_apis_per_network;
    uint8_t refresh_min;
    uint8_t network_count;
    int8_t last_success_network_idx;
    int8_t last_success_api_idx;
    uint32_t write_counter;
    /* Stored as raw uint8_t and cast to eink_panel_variant_t on read after
       validation accepts the value. */
    uint8_t panel_variant;
    /* BW per-region partial cap, clamped to
       [DASH_MAX_PARTIALS_MIN, DASH_MAX_PARTIALS_MAX]. */
    uint8_t max_partials;
    dash_wifi_profile_t networks[MAX_WIFI_NETWORKS];
} dash_config_v2_t;

void storage_init(void);
/* Returns true when a persisted config was successfully loaded from NVS —
   either the v6 per-network blobs, or a legacy v2-v5 single blob (which is
   then migrated to v6 in place). Returns false when the caller is looking
   at storage_cfg_v2_defaults() (nothing stored, or what is stored failed
   length/CRC/value validation). Existing callers may safely ignore the
   return value; passing it on is how callers distinguish a real persisted
   config from defaults. */
bool storage_load_v2(dash_config_v2_t *cfg);
esp_err_t storage_save_v2(dash_config_v2_t *cfg);
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

/* WiFi regulatory country code, persisted under its own NVS key (not the cfg_v2
 * blob). storage_get_wifi_country writes a NUL-terminated 2-char code into `out`
 * (out_sz must be >= 3), returning the portal-chosen value or, when none is
 * saved/valid, the build-stamped CONFIG_DEVDASH_WIFI_COUNTRY (falling back to
 * "01" if that is malformed). storage_set_wifi_country validates (via
 * wifi_country_is_supported in runtime_policy) and persists a new choice. */
esp_err_t storage_get_wifi_country(char *out, size_t out_sz);
esp_err_t storage_set_wifi_country(const char *cc);
/* True iff an explicit wifi_cc key exists in NVS (vs. falling back to the build
 * default). Lets the portal roll a failed save back to the *exact* prior state —
 * restoring the old value, or erasing the key when none existed. */
bool storage_wifi_country_is_saved(void);
esp_err_t storage_clear_wifi_country(void);
