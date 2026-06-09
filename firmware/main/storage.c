#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_attr.h"
#include "sdkconfig.h"
#include "runtime_policy.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

static const char *TAG = "storage";

/* Legacy single-blob key (config versions 2-5). Only read for migration;
   erased after a successful v6 save. */
static const char *CFG_V2_KEY = "cfg_v2";
/* v6 per-network layout: one small header blob plus one blob per saved
   network ("cfg_net0".."cfg_net4"). See the layout note in storage.h. */
static const char *CFG_META_KEY = "cfg_meta";

/* Fail the build closed if the panel SKU was never chosen (Gate 0.B fallback).
   Factory / SKU builds must set CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT to 0 (BWR)
   or 1 (BW); the repo dev / CI build sets 0 in sdkconfig.defaults. */
#if CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT < 0
#error "Set CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT to a panel SKU (0=BWR, 1=BW)."
#endif

eink_panel_variant_t storage_default_panel_variant(void)
{
    return (eink_panel_variant_t)CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT;
}

/* ---------------------------------------------------------------------------
 * v6 persisted layout: per-network blobs.
 *
 * `cfg_meta` carries the header fields; each `cfg_net{i}` carries one
 * network profile (with its APIs and quiet hours) plus its own version and
 * CRC, so a corrupted save costs one network instead of the whole config.
 * storage_save_v2 writes changed blobs one at a time, so the free-space
 * requirement for a re-save is old+new coexistence of a SINGLE network blob
 * (~2.9 KB), not the whole config (~14.4 KB with the pre-v6 single blob).
 * ------------------------------------------------------------------------- */

typedef struct {
    uint16_t version;             /* DASH_CFG_V2_VERSION */
    uint8_t max_wifi_networks;
    uint8_t max_apis_per_network;
    uint8_t refresh_min;
    uint8_t network_count;
    uint8_t panel_variant;
    uint8_t max_partials;
    uint32_t write_counter;
    uint32_t crc32;               /* over this struct with crc32 zeroed */
} dash_meta_blob_t;

typedef struct {
    uint16_t version;             /* DASH_CFG_V2_VERSION */
    uint16_t reserved;            /* zero */
    uint32_t crc32;               /* over this struct with crc32 zeroed */
    dash_wifi_profile_t net;
} dash_net_blob_t;

/* ---------------------------------------------------------------------------
 * Legacy v2-v5 single-blob layouts. Read-only: used to load and migrate old
 * configs. dash_wifi_profile_legacy_t must mirror the pre-v6 profile layout
 * exactly (no quiet fields — v5 kept those in trailing parallel arrays so
 * the networks[] offset stayed byte-identical with v2).
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    bool enabled;
    char ssid[DASH_SSID_MAX + 1];
    char password[DASH_WIFI_PASSWORD_MAX + 1];
    uint8_t api_count;
    dash_api_profile_t apis[MAX_APIS_PER_NETWORK];
} dash_wifi_profile_legacy_t;

/* v0.2.0-era blob (version 2): header + networks only. */
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
    dash_wifi_profile_legacy_t networks[MAX_WIFI_NETWORKS];
} dash_config_v2_legacy_t;

/* v3/v4-era blob. v3 appended panel_variant; v4's max_partials landed in
   v3's 4-byte tail padding, so sizeof(v3) == sizeof(v4) and the two are
   discriminated by the `version` field, not blob length. */
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
    dash_wifi_profile_legacy_t networks[MAX_WIFI_NETWORKS];
    uint8_t panel_variant;
    uint8_t max_partials;
} dash_config_v4_legacy_t;

/* v5 blob: v4 plus trailing parallel quiet-hours arrays (slot-indexed).
   Largest legacy layout — the migration scratch buffer is this type. */
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
    dash_wifi_profile_legacy_t networks[MAX_WIFI_NETWORKS];
    uint8_t panel_variant;
    uint8_t max_partials;
    uint8_t  quiet_enabled[MAX_WIFI_NETWORKS];
    uint16_t quiet_start_min[MAX_WIFI_NETWORKS];
    uint16_t quiet_end_min[MAX_WIFI_NETWORKS];
} dash_config_v5_legacy_t;

/* Legacy layout invariants: every version shares the byte-identical prefix
   up to networks[], so one scratch struct (v5, the largest) reads them all
   and the CRC helper can address crc32 at a single offset. */
_Static_assert(offsetof(dash_config_v5_legacy_t, crc32) ==
               offsetof(dash_config_v2_legacy_t, crc32),
               "legacy crc32 offset must match across versions");
_Static_assert(offsetof(dash_config_v5_legacy_t, networks) ==
               offsetof(dash_config_v2_legacy_t, networks),
               "legacy networks offset must match across versions");
_Static_assert(offsetof(dash_config_v5_legacy_t, crc32) ==
               offsetof(dash_config_v4_legacy_t, crc32),
               "legacy crc32 offset must match across versions");
_Static_assert(offsetof(dash_config_v5_legacy_t, networks) ==
               offsetof(dash_config_v4_legacy_t, networks),
               "legacy networks offset must match across versions");
_Static_assert(offsetof(dash_config_v5_legacy_t, panel_variant) ==
               offsetof(dash_config_v4_legacy_t, panel_variant),
               "legacy panel_variant offset must match v4/v5");
_Static_assert(offsetof(dash_config_v5_legacy_t, max_partials) ==
               offsetof(dash_config_v4_legacy_t, max_partials),
               "legacy max_partials offset must match v4/v5");
_Static_assert(sizeof(dash_config_v5_legacy_t) > sizeof(dash_config_v4_legacy_t),
               "v5 blob must be longer than v4 (length discriminates them)");
_Static_assert(sizeof(dash_config_v4_legacy_t) > sizeof(dash_config_v2_legacy_t),
               "v4 blob must be longer than legacy v2 (length discriminates them)");

/* ---------------------------------------------------------------------------
 * NVS budget. The 24 KB `nvs` partition has 6 × 4 KB pages; NVS reserves one
 * for GC, and each remaining page holds 126 × 32 B entries. The maximum
 * config (meta + 5 network blobs) plus the misc keys (ap_pwd, wifi_cc,
 * namespace bookkeeping) must leave DASH_NVS_MIN_FREE_ENTRIES free so one
 * network blob can always be rewritten (new copy while the old one still
 * exists). If these fire after a caps/size change, shrink the caps — do NOT
 * move or grow the partition (table changes do not propagate via OTA).
 * ------------------------------------------------------------------------- */
#define NVS_DATA_PAGES        5   /* 0x6000 / 0x1000 minus the reserved GC page */
#define NVS_ENTRIES_PER_PAGE  126
#define NVS_MISC_ENTRIES      32  /* ap_pwd, wifi_cc, bookkeeping, slack */

_Static_assert(sizeof(dash_api_profile_t) <= 272,
               "dash_api_profile_t grew unexpectedly — recompute blob budget");
_Static_assert(sizeof(dash_wifi_profile_t) <= 1464,
               "dash_wifi_profile_t grew unexpectedly — recompute blob budget");
_Static_assert(sizeof(dash_net_blob_t) <= DASH_NET_BLOB_MAX_BYTES,
               "network blob exceeds DASH_NET_BLOB_MAX_BYTES — shrink caps");
_Static_assert(sizeof(dash_meta_blob_t) <= 32,
               "meta blob grew unexpectedly — recompute blob budget");
_Static_assert(MAX_WIFI_NETWORKS *
               DASH_NVS_BLOB_ENTRIES(sizeof(dash_net_blob_t)) +
               DASH_NVS_BLOB_ENTRIES(sizeof(dash_meta_blob_t)) +
               NVS_MISC_ENTRIES + DASH_NVS_MIN_FREE_ENTRIES <=
               NVS_DATA_PAGES * NVS_ENTRIES_PER_PAGE,
               "max config + headroom exceeds the nvs partition budget — "
               "shrink URL/token/network caps");
_Static_assert(DASH_NVS_MIN_FREE_ENTRIES >=
               DASH_NVS_BLOB_ENTRIES(DASH_NET_BLOB_MAX_BYTES) +
               DASH_NVS_BLOB_ENTRIES(sizeof(dash_meta_blob_t)) + 8,
               "headroom must cover one network blob rewrite + meta + slack");

/* CRC32 over `size` bytes of `buf` with the 4-byte CRC field at `crc_off`
   treated as zero — piecewise, so no struct copy lands on the stack. */
static uint32_t crc_with_hole(const void *buf, size_t size, size_t crc_off)
{
    const uint8_t *bytes = (const uint8_t *)buf;
    static const uint8_t zero_crc[4] = {0};
    uint32_t crc = esp_rom_crc32_le(UINT32_MAX, bytes, crc_off);
    crc = esp_rom_crc32_le(crc, zero_crc, sizeof(zero_crc));
    crc = esp_rom_crc32_le(crc, bytes + crc_off + sizeof(zero_crc),
                           size - crc_off - sizeof(zero_crc));
    return crc;
}

/* Legacy blob CRC: same "crc32 field zeroed" scheme, evaluated over the
   stored blob size of the version at hand (the prefix offsets are shared,
   asserted above). */
static uint32_t legacy_crc(const dash_config_v5_legacy_t *cfg, size_t size)
{
    return crc_with_hole(cfg, size, offsetof(dash_config_v5_legacy_t, crc32));
}

static uint8_t clamp_refresh(uint8_t value, uint8_t panel_variant,
                             uint8_t max_partials)
{
    uint8_t minimum = dashboard_refresh_minimum(
        panel_variant == EINK_PANEL_WEACT_29_BW, max_partials);
    if (value < minimum) return minimum;
    if (value > DASH_REFRESH_MAX) return DASH_REFRESH_MAX;
    return value;
}

static uint8_t clamp_max_partials(uint8_t value)
{
    if (value < DASH_MAX_PARTIALS_MIN) return DASH_MAX_PARTIALS_MIN;
    if (value > DASH_MAX_PARTIALS_MAX) return DASH_MAX_PARTIALS_MAX;
    return value;
}

static bool panel_variant_is_known(uint8_t v)
{
    return v == EINK_PANEL_WEACT_29_BWR || v == EINK_PANEL_WEACT_29_BW;
}

static void net_key(char *buf, size_t sz, unsigned idx)
{
    snprintf(buf, sz, "cfg_net%u", idx);
}

/* ---- v6 blob acceptance ------------------------------------------------- */

static bool meta_is_valid(const dash_meta_blob_t *m)
{
    if (!m) return false;
    if (m->version != DASH_CFG_V2_VERSION) return false;
    if (m->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (m->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (m->network_count > MAX_WIFI_NETWORKS) return false;
    if (!panel_variant_is_known(m->panel_variant)) return false;
    if (m->max_partials < DASH_MAX_PARTIALS_MIN ||
        m->max_partials > DASH_MAX_PARTIALS_MAX) return false;
    if (!dashboard_refresh_config_is_valid(
            m->refresh_min,
            m->panel_variant == EINK_PANEL_WEACT_29_BW,
            m->max_partials)) return false;
    return m->crc32 == crc_with_hole(m, sizeof(*m),
                                     offsetof(dash_meta_blob_t, crc32));
}

static bool net_blob_is_valid(const dash_net_blob_t *b)
{
    if (!b) return false;
    if (b->version != DASH_CFG_V2_VERSION) return false;
    if (b->net.api_count > MAX_APIS_PER_NETWORK) return false;
    if (b->net.quiet_enabled > 1) return false;
    if (b->net.quiet_start_min > DASH_QUIET_MIN_OF_DAY_MAX) return false;
    if (b->net.quiet_end_min > DASH_QUIET_MIN_OF_DAY_MAX) return false;
    return b->crc32 == crc_with_hole(b, sizeof(*b),
                                     offsetof(dash_net_blob_t, crc32));
}

/* ---- Legacy blob acceptance (migration read path) ----------------------- */

static bool legacy_common_is_valid(const dash_config_v5_legacy_t *cfg,
                                   uint16_t expect_version)
{
    if (!cfg) return false;
    if (cfg->version != expect_version) return false;
    if (cfg->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (cfg->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (cfg->network_count > MAX_WIFI_NETWORKS) return false;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].api_count > MAX_APIS_PER_NETWORK) return false;
    }
    return true;
}

static bool legacy_v5_is_valid(const dash_config_v5_legacy_t *cfg)
{
    if (!legacy_common_is_valid(cfg, 5)) return false;
    if (!panel_variant_is_known(cfg->panel_variant)) return false;
    if (cfg->max_partials < DASH_MAX_PARTIALS_MIN ||
        cfg->max_partials > DASH_MAX_PARTIALS_MAX) return false;
    if (!dashboard_refresh_config_is_valid(
            cfg->refresh_min,
            cfg->panel_variant == EINK_PANEL_WEACT_29_BW,
            cfg->max_partials)) return false;
    for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (cfg->quiet_enabled[i] > 1) return false;
        if (cfg->quiet_start_min[i] > DASH_QUIET_MIN_OF_DAY_MAX) return false;
        if (cfg->quiet_end_min[i] > DASH_QUIET_MIN_OF_DAY_MAX) return false;
    }
    return cfg->crc32 == legacy_crc(cfg, sizeof(dash_config_v5_legacy_t));
}

static bool legacy_v4_is_valid(const dash_config_v5_legacy_t *cfg)
{
    if (!legacy_common_is_valid(cfg, 4)) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    if (!panel_variant_is_known(cfg->panel_variant)) return false;
    if (cfg->max_partials < DASH_MAX_PARTIALS_MIN ||
        cfg->max_partials > DASH_MAX_PARTIALS_MAX) return false;
    return cfg->crc32 == legacy_crc(cfg, sizeof(dash_config_v4_legacy_t));
}

/* v3 arrives at the v4 length (shared size, see the struct note). Do NOT
   inspect the max_partials byte — v3 never required it to hold any
   particular value; the migration overwrites it with the default. */
static bool legacy_v3_is_valid(const dash_config_v5_legacy_t *cfg)
{
    if (!legacy_common_is_valid(cfg, 3)) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    if (!panel_variant_is_known(cfg->panel_variant)) return false;
    return cfg->crc32 == legacy_crc(cfg, sizeof(dash_config_v4_legacy_t));
}

static bool legacy_v2_is_valid(const dash_config_v5_legacy_t *cfg)
{
    if (!legacy_common_is_valid(cfg, 2)) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    return cfg->crc32 == legacy_crc(cfg, sizeof(dash_config_v2_legacy_t));
}

/* Reconnect hints kept in RTC memory instead of NVS — see the storage.h
   header for the full rationale (NVS churn / fragmentation). These survive
   deep sleep and esp_restart; on a cold boot they start at -1 ("no hint"),
   which makes the device fall back to a normal scan. Even if a given
   chip/reset path were to zero-init these instead of honoring the -1, idx 0 is
   a valid in-range network, so the worst case is "try network 0 first" — a
   benign reconnect order, and storage_apply_last_success clamps it anyway. */
static RTC_DATA_ATTR int8_t s_last_success_net = -1;
static RTC_DATA_ATTR int8_t s_last_success_api = -1;

void storage_note_last_success(int8_t net_idx, int8_t api_idx)
{
    /* RTC write only — no flash, so no need to skip unchanged values. */
    s_last_success_net = net_idx;
    s_last_success_api = api_idx;
}

void storage_apply_last_success(dash_config_v2_t *cfg)
{
    if (!cfg) return;
    cfg->last_success_network_idx = s_last_success_net;
    cfg->last_success_api_idx = s_last_success_api;
    /* Pair-clamp against the freshly loaded config: the RTC hints may point at
       a network/API the user has since deleted or reordered via the portal.
       An api index is meaningless without a valid network, so reset BOTH to -1
       if either is out of range. Mirrors the invariant in
       storage_cfg_v2_normalize, but runs right after load before first use. */
    if (cfg->last_success_network_idx < 0 ||
        cfg->last_success_network_idx >= (int8_t)cfg->network_count) {
        cfg->last_success_network_idx = -1;
        cfg->last_success_api_idx = -1;
    } else {
        uint8_t api_count =
            cfg->networks[(uint8_t)cfg->last_success_network_idx].api_count;
        if (cfg->last_success_api_idx >= (int8_t)api_count) {
            cfg->last_success_api_idx = -1;
        }
    }
    /* Keep RTC consistent with the clamped values so later reads agree. */
    s_last_success_net = cfg->last_success_network_idx;
    s_last_success_api = cfg->last_success_api_idx;
}

void storage_cfg_v2_defaults(dash_config_v2_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = DASH_CFG_V2_VERSION;
    cfg->max_wifi_networks = MAX_WIFI_NETWORKS;
    cfg->max_apis_per_network = MAX_APIS_PER_NETWORK;
    cfg->last_success_network_idx = -1;
    cfg->last_success_api_idx = -1;
    cfg->panel_variant = storage_default_panel_variant();
    cfg->max_partials = DASH_MAX_PARTIALS_DEFAULT;
    cfg->refresh_min = clamp_refresh(CONFIG_DEVDASH_REFRESH_MIN,
                                     cfg->panel_variant, cfg->max_partials);
}

bool storage_validate_api_url(const char *url)
{
    if (!url || url[0] == '\0') return false;
    const char *host;
    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else {
        return false;
    }
    size_t len = strlen(url);
    if (len >= DASH_API_URL_MAX) return false;
    if (*host == '\0') return false;
    /* Query strings are intentionally rejected because this is a base URL. */
    for (const char *p = host; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == ':' || c == '/' || c == '_';
        if (!ok) return false;
    }
    return true;
}

void storage_cfg_v2_normalize(dash_config_v2_t *cfg)
{
    cfg->version = DASH_CFG_V2_VERSION;
    cfg->max_wifi_networks = MAX_WIFI_NETWORKS;
    cfg->max_apis_per_network = MAX_APIS_PER_NETWORK;
    if (!panel_variant_is_known(cfg->panel_variant)) {
        cfg->panel_variant = storage_default_panel_variant();
    }
    cfg->max_partials = clamp_max_partials(cfg->max_partials);
    cfg->refresh_min = clamp_refresh(cfg->refresh_min, cfg->panel_variant,
                                     cfg->max_partials);
    if (cfg->network_count > MAX_WIFI_NETWORKS) cfg->network_count = MAX_WIFI_NETWORKS;

    for (uint8_t i = 0; i < cfg->network_count; i++) {
        dash_wifi_profile_t *net = &cfg->networks[i];
        net->ssid[DASH_SSID_MAX] = '\0';
        net->password[DASH_WIFI_PASSWORD_MAX] = '\0';
        if (net->id == 0) net->id = i + 1;
        if (net->api_count > MAX_APIS_PER_NETWORK) net->api_count = MAX_APIS_PER_NETWORK;
        for (uint8_t j = 0; j < net->api_count; j++) {
            dash_api_profile_t *api = &net->apis[j];
            api->api_url[DASH_API_URL_MAX - 1] = '\0';
            api->device_token[DASH_DEVICE_TOKEN_MAX - 1] = '\0';
            if (api->id == 0) api->id = ((uint32_t)net->id * 100u) + j + 1u;
        }
        memset(&net->apis[net->api_count], 0,
               sizeof(net->apis[0]) * (MAX_APIS_PER_NETWORK - net->api_count));

        /* Quiet hours: clamp times to a valid minute-of-day, coerce enabled
           to 0/1, and treat start == end as disabled. */
        if (net->quiet_start_min > DASH_QUIET_MIN_OF_DAY_MAX) {
            net->quiet_start_min = DASH_QUIET_MIN_OF_DAY_MAX;
        }
        if (net->quiet_end_min > DASH_QUIET_MIN_OF_DAY_MAX) {
            net->quiet_end_min = DASH_QUIET_MIN_OF_DAY_MAX;
        }
        net->quiet_enabled = net->quiet_enabled ? 1 : 0;
        if (net->quiet_start_min == net->quiet_end_min) {
            net->quiet_enabled = 0;
        }
    }
    /* Zero the trailing slots that have no network, so the per-slot blob
       compare in storage_save_v2 sees stable bytes. */
    memset(&cfg->networks[cfg->network_count], 0,
           sizeof(cfg->networks[0]) * (MAX_WIFI_NETWORKS - cfg->network_count));

    if (cfg->last_success_network_idx >= (int8_t)cfg->network_count) {
        cfg->last_success_network_idx = -1;
    }
    if (cfg->last_success_network_idx < 0) {
        cfg->last_success_api_idx = -1;
    } else {
        uint8_t api_count = cfg->networks[(uint8_t)cfg->last_success_network_idx].api_count;
        if (cfg->last_success_api_idx >= (int8_t)api_count) {
            cfg->last_success_api_idx = -1;
        }
    }
}

uint32_t storage_next_profile_id(const dash_config_v2_t *cfg)
{
    uint32_t max_id = 0;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].id > max_id) max_id = cfg->networks[i].id;
        for (uint8_t j = 0; j < cfg->networks[i].api_count; j++) {
            if (cfg->networks[i].apis[j].id > max_id) max_id = cfg->networks[i].apis[j].id;
        }
    }
    return max_id + 1;
}

void storage_mask_token(const char *token, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (!token || token[0] == '\0') {
        out[0] = '\0';
        return;
    }
    size_t len = strlen(token);
    const char *last = len > 4 ? token + len - 4 : token;
    snprintf(out, out_sz, "****%s", last);
}

void storage_init(void)
{
    /* nvs_flash_init() is called in app_main before storage_init */
    ESP_LOGI(TAG, "cfg storage: per-network blobs (net %u B, meta %u B, "
             "headroom %u entries)",
             (unsigned)sizeof(dash_net_blob_t),
             (unsigned)sizeof(dash_meta_blob_t),
             (unsigned)DASH_NVS_MIN_FREE_ENTRIES);
}

/* Load the v6 per-network layout. Returns true when the meta blob validates;
   individual network blobs that are missing or corrupt are logged and
   dropped (the surviving networks compact down), which is the
   blast-radius win of the per-network split. */
static bool load_v6(nvs_handle_t h, dash_config_v2_t *cfg)
{
    dash_meta_blob_t meta = {0};
    size_t len = sizeof(meta);
    esp_err_t err = nvs_get_blob(h, CFG_META_KEY, &meta, &len);
    if (err != ESP_OK || len != sizeof(meta) || !meta_is_valid(&meta)) {
        ESP_LOGW(TAG, "Ignoring invalid cfg_meta blob");
        return false;
    }

    cfg->refresh_min = meta.refresh_min;
    cfg->panel_variant = meta.panel_variant;
    cfg->max_partials = meta.max_partials;
    cfg->write_counter = meta.write_counter;

    dash_net_blob_t *blob = calloc(1, sizeof(*blob));
    if (!blob) return false;
    uint8_t out = 0;
    for (uint8_t i = 0; i < meta.network_count; i++) {
        char key[16];
        net_key(key, sizeof(key), i);
        size_t blen = sizeof(*blob);
        memset(blob, 0, sizeof(*blob));
        err = nvs_get_blob(h, key, blob, &blen);
        if (err != ESP_OK || blen != sizeof(*blob) || !net_blob_is_valid(blob)) {
            ESP_LOGW(TAG, "Dropping missing/invalid network blob %s", key);
            continue;
        }
        cfg->networks[out++] = blob->net;
    }
    free(blob);
    cfg->network_count = out;
    return true;
}

/* Copy a validated legacy blob into the current in-RAM layout. The scratch
   struct is zero-filled before the NVS read, so for pre-v5 blobs the quiet
   arrays (and for pre-v3 the panel/partials bytes) read back as zero; the
   caller fixes up the per-version defaults afterwards. */
static void legacy_to_cfg(const dash_config_v5_legacy_t *old,
                          dash_config_v2_t *cfg)
{
    storage_cfg_v2_defaults(cfg);
    cfg->refresh_min = old->refresh_min;
    cfg->network_count = old->network_count;
    cfg->last_success_network_idx = old->last_success_network_idx;
    cfg->last_success_api_idx = old->last_success_api_idx;
    cfg->write_counter = old->write_counter;
    cfg->panel_variant = old->panel_variant;
    cfg->max_partials = old->max_partials;
    for (uint8_t i = 0; i < old->network_count; i++) {
        const dash_wifi_profile_legacy_t *src = &old->networks[i];
        dash_wifi_profile_t *dst = &cfg->networks[i];
        dst->id = src->id;
        dst->enabled = src->enabled;
        memcpy(dst->ssid, src->ssid, sizeof(dst->ssid));
        memcpy(dst->password, src->password, sizeof(dst->password));
        dst->api_count = src->api_count;
        memcpy(dst->apis, src->apis, sizeof(dst->apis));
        dst->quiet_enabled = old->quiet_enabled[i];
        dst->quiet_start_min = old->quiet_start_min[i];
        dst->quiet_end_min = old->quiet_end_min[i];
    }
}

/* One-time cleanup after a successful legacy → v6 migration. The legacy
   cfg_v2 key is already erased by the successful storage_save_v2; what
   remains is the WiFi driver's nvs.net80211 namespace — dead weight in the
   same 24 KB partition now that the driver runs with WIFI_STORAGE_RAM (see
   wifi_net_init): the app owns all credentials in cfg and reconfigures the
   driver from it on every boot. Erasing it reclaims its entries for the
   per-network blobs. */
static void migrate_cleanup_wifi_namespace(void)
{
    nvs_handle_t h;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erased stale nvs.net80211 namespace");
    } else {
        ESP_LOGW(TAG, "nvs.net80211 cleanup failed: %s", esp_err_to_name(err));
    }
}

bool storage_load_v2(dash_config_v2_t *cfg)
{
    storage_cfg_v2_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    /* v6 path: presence of the meta blob is authoritative — once it exists,
       the legacy key (if any survived a failed cleanup) is never consulted
       again. */
    size_t meta_len = 0;
    if (nvs_get_blob(h, CFG_META_KEY, NULL, &meta_len) == ESP_OK) {
        bool ok = load_v6(h, cfg);
        nvs_close(h);
        if (!ok) storage_cfg_v2_defaults(cfg);
        storage_cfg_v2_normalize(cfg);
        return ok;
    }

    /* No meta blob: look for a legacy v2-v5 single blob and migrate it. */
    size_t blob_len = 0;
    esp_err_t err = nvs_get_blob(h, CFG_V2_KEY, NULL, &blob_len);
    if (err != ESP_OK) {
        nvs_close(h);
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    /* The scratch struct is the largest legacy layout; ~7.3 KB, so it lives
       on the heap — never on the caller's stack. */
    dash_config_v5_legacy_t *old = calloc(1, sizeof(*old));
    if (!old) {
        nvs_close(h);
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    bool ok = false;
    uint16_t from_version = 0;
    if (blob_len == sizeof(dash_config_v5_legacy_t) ||
        blob_len == sizeof(dash_config_v4_legacy_t) ||
        blob_len == sizeof(dash_config_v2_legacy_t)) {
        size_t len = blob_len;
        err = nvs_get_blob(h, CFG_V2_KEY, old, &len);
        if (err == ESP_OK && len == blob_len) {
            if (blob_len == sizeof(dash_config_v5_legacy_t) &&
                legacy_v5_is_valid(old)) {
                legacy_to_cfg(old, cfg);
                ok = true;
            } else if (blob_len == sizeof(dash_config_v4_legacy_t) &&
                       legacy_v4_is_valid(old)) {
                legacy_to_cfg(old, cfg);
                ok = true;
            } else if (blob_len == sizeof(dash_config_v4_legacy_t) &&
                       legacy_v3_is_valid(old)) {
                legacy_to_cfg(old, cfg);
                /* v3's max_partials position held an arbitrary tail-padding
                   byte; overwrite it with the default. */
                cfg->max_partials = DASH_MAX_PARTIALS_DEFAULT;
                ok = true;
            } else if (blob_len == sizeof(dash_config_v2_legacy_t) &&
                       legacy_v2_is_valid(old)) {
                legacy_to_cfg(old, cfg);
                /* Legacy v2 blobs carry neither field. Seed the SKU default
                   panel variant (so an upgraded BW-SKU device resolves to BW,
                   not a hardcoded BWR) and the default partial cap. */
                cfg->panel_variant = storage_default_panel_variant();
                cfg->max_partials = DASH_MAX_PARTIALS_DEFAULT;
                ok = true;
            }
            from_version = old->version;
        }
    } else {
        ESP_LOGW(TAG, "Unknown cfg_v2 blob length %u; resetting to defaults",
                 (unsigned)blob_len);
    }
    free(old);
    nvs_close(h);

    if (!ok) {
        ESP_LOGW(TAG, "Ignoring invalid legacy cfg_v2 blob");
        storage_cfg_v2_defaults(cfg);
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    ESP_LOGW(TAG, "cfg_v2 v%u: migrating to per-network blobs (v%u)",
             from_version, DASH_CFG_V2_VERSION);
    /* storage_save_v2 writes the v6 blobs and erases the legacy key once the
       new layout is fully committed. The old ~7.2 KB blob and the new blobs
       must briefly coexist; on a fragmented partition that can fail with
       NOT_ENOUGH_SPACE, so fall back to erasing the old blob first (the
       config is safe in RAM) and retry once. The fallback has a power-loss
       window — accepted, because without it the migration cannot proceed at
       all on exactly the partitions this change is rescuing. */
    esp_err_t save_err = storage_save_v2(cfg);
    if (save_err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "Migration save needs space: erasing legacy blob first");
        nvs_handle_t hw;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
            if (nvs_erase_key(hw, CFG_V2_KEY) == ESP_OK) nvs_commit(hw);
            nvs_close(hw);
        }
        save_err = storage_save_v2(cfg);
    }
    if (save_err == ESP_OK) {
        migrate_cleanup_wifi_namespace();
    } else {
        ESP_LOGW(TAG, "v%u→v%u migration save failed (err=0x%x); "
                 "using in-memory cfg", from_version, DASH_CFG_V2_VERSION,
                 save_err);
        storage_cfg_v2_normalize(cfg);
    }
    return true;
}

esp_err_t storage_save_v2(dash_config_v2_t *cfg)
{
    /* Normalize in place. The previous version made a 7 KiB stack copy to
     * keep the API const-correct, but that doubled the call's stack
     * footprint and pushed the main task past the WDT. */
    storage_cfg_v2_normalize(cfg);

    /* Portal edits can add/remove/reorder networks, which normalize() may have
       re-clamped the last_success hints for. Keep the RTC copy in sync so the
       next wake's storage_apply_last_success sees the corrected slot rather
       than a stale one pointing at a since-deleted network. */
    s_last_success_net = cfg->last_success_network_idx;
    s_last_success_api = cfg->last_success_api_idx;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* Candidate + stored scratch blobs (~1.5 KB each) — heap, never on the
       caller's stack. */
    dash_net_blob_t *cand = calloc(1, sizeof(*cand));
    dash_net_blob_t *stored = calloc(1, sizeof(*stored));
    if (!cand || !stored) {
        free(cand);
        free(stored);
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    /* Runtime guard (defense-in-depth; statically the budget asserts make
       this unreachable): refuse to start a write sequence when fewer than
       DASH_NVS_MIN_FREE_ENTRIES are reclaimable, so the portal shows a clear
       error instead of a half-finished save. total - used counts
       erased-but-not-yet-GCed entries, which NVS reclaims via the reserved
       spare page when a write needs them. */
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        size_t reclaimable = stats.total_entries - stats.used_entries;
        if (reclaimable < DASH_NVS_MIN_FREE_ENTRIES) {
            ESP_LOGE(TAG, "NVS nearly full (%u/%u entries used) — refusing "
                     "save; need %u free",
                     (unsigned)stats.used_entries,
                     (unsigned)stats.total_entries,
                     (unsigned)DASH_NVS_MIN_FREE_ENTRIES);
            free(cand);
            free(stored);
            nvs_close(h);
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
    }

    /* Write order:
       1. erase stale per-network slots (frees entries before any write),
       2. per-network blobs ONE AT A TIME, skipping unchanged ones — peak
          old+new coexistence is a single ~1.5 KB blob no matter how many
          networks changed,
       3. meta last, so a power cut mid-sequence leaves the old meta
          pointing at individually-valid blobs (load drops at worst a
          missing slot),
       4. legacy cfg_v2 key erase, only after the v6 layout fully landed.
       A failed step aborts the sequence; since unchanged blobs are skipped,
       a retry resumes idempotently where it stopped. */
    bool changed = false;
    char key[16];

    for (uint8_t i = cfg->network_count; i < MAX_WIFI_NETWORKS && err == ESP_OK; i++) {
        net_key(key, sizeof(key), i);
        esp_err_t eerr = nvs_erase_key(h, key);
        if (eerr == ESP_OK) {
            err = nvs_commit(h);
            changed = true;
        } else if (eerr != ESP_ERR_NVS_NOT_FOUND) {
            err = eerr;
        }
    }

    for (uint8_t i = 0; i < cfg->network_count && err == ESP_OK; i++) {
        memset(cand, 0, sizeof(*cand));
        cand->version = DASH_CFG_V2_VERSION;
        cand->net = cfg->networks[i];
        cand->crc32 = crc_with_hole(cand, sizeof(*cand),
                                    offsetof(dash_net_blob_t, crc32));
        net_key(key, sizeof(key), i);
        size_t blen = sizeof(*stored);
        memset(stored, 0, sizeof(*stored));
        esp_err_t gerr = nvs_get_blob(h, key, stored, &blen);
        if (gerr == ESP_OK && blen == sizeof(*stored) &&
            memcmp(stored, cand, sizeof(*cand)) == 0) {
            continue; /* unchanged — no rewrite, no wear */
        }
        err = nvs_set_blob(h, key, cand, sizeof(*cand));
        if (err == ESP_OK) err = nvs_commit(h);
        if (err == ESP_OK) changed = true;
    }

    if (err == ESP_OK) {
        dash_meta_blob_t meta = {0};
        meta.version = DASH_CFG_V2_VERSION;
        meta.max_wifi_networks = MAX_WIFI_NETWORKS;
        meta.max_apis_per_network = MAX_APIS_PER_NETWORK;
        meta.refresh_min = cfg->refresh_min;
        meta.network_count = cfg->network_count;
        meta.panel_variant = cfg->panel_variant;
        meta.max_partials = cfg->max_partials;

        dash_meta_blob_t prev = {0};
        size_t mlen = sizeof(prev);
        bool have_prev = nvs_get_blob(h, CFG_META_KEY, &prev, &mlen) == ESP_OK &&
                         mlen == sizeof(prev) && meta_is_valid(&prev);
        bool meta_differs = !have_prev ||
            prev.refresh_min != meta.refresh_min ||
            prev.network_count != meta.network_count ||
            prev.panel_variant != meta.panel_variant ||
            prev.max_partials != meta.max_partials;
        if (changed || meta_differs) {
            meta.write_counter =
                (have_prev ? prev.write_counter : cfg->write_counter) + 1;
            meta.crc32 = crc_with_hole(&meta, sizeof(meta),
                                       offsetof(dash_meta_blob_t, crc32));
            err = nvs_set_blob(h, CFG_META_KEY, &meta, sizeof(meta));
            if (err == ESP_OK) err = nvs_commit(h);
            if (err == ESP_OK) cfg->write_counter = meta.write_counter;
        }
        /* A fully unchanged save writes nothing at all (the pre-v6 code
           rewrote the whole 7.2 KB blob even for a no-op portal save). */
    }

    if (err == ESP_OK) {
        /* The v6 layout is fully committed; the legacy blob (if any) is now
           redundant. NOT_FOUND is the normal steady state. */
        esp_err_t eerr = nvs_erase_key(h, CFG_V2_KEY);
        if (eerr == ESP_OK) {
            nvs_commit(h);
        } else if (eerr != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "legacy cfg_v2 erase failed: %s",
                     esp_err_to_name(eerr));
        }
    }

    free(cand);
    free(stored);
    nvs_close(h);
    return err;
}

void storage_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t storage_get_or_init_ap_password(char *out, size_t out_sz)
{
    static const char ALNUM[62] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789";
    static const char *KEY = "ap_pwd";
    enum { AP_PWD_LEN = 12 };

    if (!out || out_sz < AP_PWD_LEN + 1) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t len = out_sz;
    err = nvs_get_str(h, KEY, out, &len);
    if (err == ESP_OK && len >= AP_PWD_LEN + 1) {
        nvs_close(h);
        return ESP_OK;
    }

    uint8_t rnd[AP_PWD_LEN];
    extern void esp_fill_random(void *buf, size_t len);
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < AP_PWD_LEN; i++) out[i] = ALNUM[rnd[i] % 62];
    out[AP_PWD_LEN] = '\0';

    err = nvs_set_str(h, KEY, out);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* WiFi regulatory country code. Stored as its own small NVS string key — NOT
 * part of the config blobs — so the portal can change the regulatory domain
 * without churning the config writes, mirroring how the SoftAP password is
 * persisted. Accepts the two world-safe "01" sentinel or a
 * two-uppercase-letter ISO code; anything else is rejected so a crafted POST
 * cannot push an invalid string into esp_wifi_set_country_code(). */
#define WIFI_CC_KEY "wifi_cc"

esp_err_t storage_get_wifi_country(char *out, size_t out_sz)
{
    if (!out || out_sz < 3) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_sz;
        esp_err_t err = nvs_get_str(h, WIFI_CC_KEY, out, &len);
        nvs_close(h);
        if (err == ESP_OK && wifi_country_is_supported(out)) return ESP_OK;
    }

    /* No valid saved choice yet — fall back to the build-stamped default, and to
     * the world-safe "01" if even that is malformed (guards a bad
     * CONFIG_DEVDASH_WIFI_COUNTRY from reaching esp_wifi_set_country_code). */
    const char *def = CONFIG_DEVDASH_WIFI_COUNTRY;
    if (!wifi_country_is_supported(def)) def = "01";
    strncpy(out, def, out_sz - 1);
    out[out_sz - 1] = '\0';
    return ESP_OK;
}

bool storage_wifi_country_is_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char buf[4] = {0};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, WIFI_CC_KEY, buf, &len);
    nvs_close(h);
    return err == ESP_OK;
}

esp_err_t storage_clear_wifi_country(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, WIFI_CC_KEY);
    /* Absent key is success: the post-condition (no saved override) holds. */
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_set_wifi_country(const char *cc)
{
    if (!wifi_country_is_supported(cc)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, WIFI_CC_KEY, cc);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
