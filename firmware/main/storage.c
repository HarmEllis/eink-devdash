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

static const char *TAG = "storage";
static const char *CFG_V2_KEY = "cfg_v2";

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

/* Legacy struct used to read v0.2.0-era cfg_v2 blobs from NVS. Must mirror
   the pre-v3 dash_config_v2_t layout exactly. v3 appended uint8_t panel_variant
   and v4 appended uint8_t max_partials; both are trailing fields, so every
   prefix offset is byte-identical with this legacy struct — that's the
   migration invariant the static assertions below pin down. */
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
} dash_config_v2_legacy_t;

/* Pre-v5 struct (v3 + v4 era). Used to read and migrate old blobs: v3/v4 both
   stored at this size (max_partials landed in v3's tail padding, so
   sizeof(v3) == sizeof(v4)). v5 appends the quiet-hours arrays, which do NOT
   fit in tail padding, so sizeof(dash_config_v2_t) > sizeof(dash_config_v4_t).
   The prefix up to and including max_partials is byte-identical with v5 — the
   asserts below pin that invariant so an L4 blob overlays the v5 struct prefix
   cleanly while the trailing quiet arrays stay zero-seeded (= disabled). */
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
    uint8_t panel_variant;
    uint8_t max_partials;
} dash_config_v4_t;

_Static_assert(sizeof(dash_api_profile_t) <= 272,
               "dash_api_profile_t grew unexpectedly — recompute blob budget");
_Static_assert(sizeof(dash_wifi_profile_t) <= 1464,
               "dash_wifi_profile_t grew unexpectedly — recompute blob budget");
_Static_assert(sizeof(dash_config_v2_t) <= DASH_CFG_V2_MAX_BYTES,
               "cfg_v2 exceeds DASH_CFG_V2_MAX_BYTES — switch to per-network blobs");
_Static_assert(offsetof(dash_config_v2_t, crc32) ==
               offsetof(dash_config_v2_legacy_t, crc32),
               "v3 crc32 offset must match v2 — migration invariant");
_Static_assert(offsetof(dash_config_v2_t, networks) ==
               offsetof(dash_config_v2_legacy_t, networks),
               "v3 networks offset must match v2 — migration invariant");
_Static_assert(sizeof(dash_config_v2_t) > sizeof(dash_config_v2_legacy_t),
               "v3/v4/v5 struct must be larger than legacy v2 (added trailing fields)");
/* NOTE: max_partials landed in v3's 4-byte tail padding after panel_variant, so
   sizeof(v4) == sizeof(v3); both read as dash_config_v4_t and are discriminated
   by the `version` field, not blob length. v5 appends the quiet-hours arrays,
   which do NOT fit in padding, so sizeof(v5) > sizeof(v4): the load path tells
   v5 from v3/v4 by blob length, then v3 from v4 by `version`. */
_Static_assert(offsetof(dash_config_v2_t, networks) ==
               offsetof(dash_config_v4_t, networks),
               "v5 networks offset must match v4 — migration invariant");
_Static_assert(offsetof(dash_config_v2_t, crc32) ==
               offsetof(dash_config_v4_t, crc32),
               "v5 crc32 offset must match v4 — migration invariant");
_Static_assert(offsetof(dash_config_v2_t, panel_variant) ==
               offsetof(dash_config_v4_t, panel_variant),
               "v5 panel_variant offset must match v4 — migration invariant");
_Static_assert(offsetof(dash_config_v2_t, max_partials) ==
               offsetof(dash_config_v4_t, max_partials),
               "v5 max_partials offset must match v4 — migration invariant");
_Static_assert(sizeof(dash_config_v2_t) > sizeof(dash_config_v4_t),
               "v5 struct must be larger than v4 (quiet arrays exceed tail padding)");
_Static_assert(sizeof(dash_config_v4_t) > sizeof(dash_config_v2_legacy_t),
               "v4 struct must be larger than legacy v2 (panel_variant/max_partials)");
/* Caps×counts budget: keeps the comfort margin obvious if caps move. */
_Static_assert((size_t)DASH_API_URL_MAX * MAX_APIS_PER_NETWORK * MAX_WIFI_NETWORKS +
               (size_t)DASH_DEVICE_TOKEN_MAX * MAX_APIS_PER_NETWORK * MAX_WIFI_NETWORKS <=
               DASH_CFG_V2_MAX_BYTES - 512,
               "URL/token caps × networks × APIs leave <512 B for headers — "
               "lower caps or switch to per-network blobs");

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

/* Piecewise CRC over the first `size` bytes of the struct with the crc32 field
   treated as zero, matching the original "copy struct, zero crc32 field, CRC the
   whole thing" semantics — without a 7 KiB stack copy that would overflow the
   default main task stack. crc_off is identical across v2/v3/v4/v5 (shared
   prefix, asserted above), so the same routine validates every version by
   passing its stored blob size. */
static uint32_t cfg_crc_sized(const dash_config_v2_t *cfg, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)cfg;
    const size_t crc_off = offsetof(dash_config_v2_t, crc32);
    static const uint8_t zero_crc[sizeof(((dash_config_v2_t *)0)->crc32)] = {0};
    uint32_t crc = esp_rom_crc32_le(UINT32_MAX, bytes, crc_off);
    crc = esp_rom_crc32_le(crc, zero_crc, sizeof(zero_crc));
    crc = esp_rom_crc32_le(crc, bytes + crc_off + sizeof(zero_crc),
                           size - crc_off - sizeof(zero_crc));
    return crc;
}

/* Current (v5) CRC over the full struct. */
static uint32_t cfg_crc(const dash_config_v2_t *cfg)
{
    return cfg_crc_sized(cfg, sizeof(dash_config_v2_t));
}

/* CRC over the pre-v5 (v3/v4) blob size — both stored at sizeof(dash_config_v4_t)
   because the prefix is byte-identical (asserted above). */
static uint32_t cfg_crc_v4(const dash_config_v2_t *cfg)
{
    return cfg_crc_sized(cfg, sizeof(dash_config_v4_t));
}

/* CRC over the legacy v2 blob size (no panel_variant / max_partials). */
static uint32_t cfg_crc_legacy(const dash_config_v2_t *cfg)
{
    return cfg_crc_sized(cfg, sizeof(dash_config_v2_legacy_t));
}

static bool panel_variant_is_known(uint8_t v)
{
    return v == EINK_PANEL_WEACT_29_BWR || v == EINK_PANEL_WEACT_29_BW;
}

/* Current-version (v5) blob acceptance. Validates the quiet-hours arrays in
   addition to the v4 fields; the whole-struct cfg_crc covers the trailing
   arrays. */
static bool cfg_v5_is_valid(const dash_config_v2_t *cfg)
{
    if (!cfg) return false;
    if (cfg->version != DASH_CFG_V2_VERSION) return false;
    if (cfg->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (cfg->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (cfg->network_count > MAX_WIFI_NETWORKS) return false;
    if (!panel_variant_is_known(cfg->panel_variant)) return false;
    if (cfg->max_partials < DASH_MAX_PARTIALS_MIN ||
        cfg->max_partials > DASH_MAX_PARTIALS_MAX) return false;
    if (!dashboard_refresh_config_is_valid(
            cfg->refresh_min,
            cfg->panel_variant == EINK_PANEL_WEACT_29_BW,
            cfg->max_partials)) return false;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].api_count > MAX_APIS_PER_NETWORK) return false;
    }
    for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (cfg->quiet_enabled[i] > 1) return false;
        if (cfg->quiet_start_min[i] > DASH_QUIET_MIN_OF_DAY_MAX) return false;
        if (cfg->quiet_end_min[i] > DASH_QUIET_MIN_OF_DAY_MAX) return false;
    }
    return cfg->crc32 == cfg_crc(cfg);
}

/* v4 blob acceptance for in-place v4->v5 migration. v4 stored at
   sizeof(dash_config_v4_t) (< sizeof v5), so its CRC covers only that prefix;
   validate with cfg_crc_v4. The migration zero-seeds the quiet arrays. */
static bool cfg_v4_is_valid(const dash_config_v2_t *cfg)
{
    if (!cfg) return false;
    if (cfg->version != 4) return false;
    if (cfg->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (cfg->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (cfg->network_count > MAX_WIFI_NETWORKS) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    if (!panel_variant_is_known(cfg->panel_variant)) return false;
    if (cfg->max_partials < DASH_MAX_PARTIALS_MIN ||
        cfg->max_partials > DASH_MAX_PARTIALS_MAX) return false;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].api_count > MAX_APIS_PER_NETWORK) return false;
    }
    return cfg->crc32 == cfg_crc_v4(cfg);
}

/* v3 blob acceptance for in-place v3->v5 migration. sizeof(v3) == sizeof(v4),
   so a v3 blob arrives at the v4 length but carries version==3 with the
   max_partials position holding whatever tail-padding byte v3 wrote (the v3 CRC
   covered it, but v3 never required it to be any particular value). We validate
   with cfg_crc_v4 (the v3/v4 size) and do NOT inspect the max_partials byte —
   the migration overwrites it with the default regardless. */
static bool cfg_v3_is_valid(const dash_config_v2_t *cfg)
{
    if (!cfg) return false;
    if (cfg->version != 3) return false;
    if (cfg->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (cfg->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (cfg->network_count > MAX_WIFI_NETWORKS) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    if (!panel_variant_is_known(cfg->panel_variant)) return false;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].api_count > MAX_APIS_PER_NETWORK) return false;
    }
    return cfg->crc32 == cfg_crc_v4(cfg);
}

static bool cfg_v2_legacy_is_valid(const dash_config_v2_t *cfg)
{
    if (!cfg) return false;
    if (cfg->version != 2) return false;
    if (cfg->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (cfg->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (cfg->network_count > MAX_WIFI_NETWORKS) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].api_count > MAX_APIS_PER_NETWORK) return false;
    }
    return cfg->crc32 == cfg_crc_legacy(cfg);
}

/* Reconnect hints kept in RTC memory instead of the NVS blob — see the
   storage.h header for the full rationale (NVS churn / fragmentation). These
   survive deep sleep and esp_restart; on a cold boot they start at -1 ("no
   hint"), which makes the device fall back to a normal scan. Even if a given
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
    }
    memset(&cfg->networks[cfg->network_count], 0,
           sizeof(cfg->networks[0]) * (MAX_WIFI_NETWORKS - cfg->network_count));

    /* Quiet-hours arrays are slot-indexed, parallel to networks[]. Clamp times
       to a valid minute-of-day, coerce enabled to 0/1, treat start==end as
       disabled, and zero the trailing slots that have no network. */
    for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (i >= cfg->network_count) {
            cfg->quiet_enabled[i] = 0;
            cfg->quiet_start_min[i] = 0;
            cfg->quiet_end_min[i] = 0;
            continue;
        }
        if (cfg->quiet_start_min[i] > DASH_QUIET_MIN_OF_DAY_MAX) {
            cfg->quiet_start_min[i] = DASH_QUIET_MIN_OF_DAY_MAX;
        }
        if (cfg->quiet_end_min[i] > DASH_QUIET_MIN_OF_DAY_MAX) {
            cfg->quiet_end_min[i] = DASH_QUIET_MIN_OF_DAY_MAX;
        }
        cfg->quiet_enabled[i] = cfg->quiet_enabled[i] ? 1 : 0;
        if (cfg->quiet_start_min[i] == cfg->quiet_end_min[i]) {
            cfg->quiet_enabled[i] = 0;
        }
    }

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
    cfg->crc32 = cfg_crc(cfg);
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
    ESP_LOGI(TAG, "cfg_v2 blob: %u bytes (%u%% of %u B budget)",
             (unsigned)sizeof(dash_config_v2_t),
             (unsigned)((sizeof(dash_config_v2_t) * 100u) / DASH_CFG_V2_MAX_BYTES),
             (unsigned)DASH_CFG_V2_MAX_BYTES);
}

bool storage_load_v2(dash_config_v2_t *cfg)
{
    storage_cfg_v2_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    size_t blob_len = 0;
    esp_err_t err = nvs_get_blob(h, CFG_V2_KEY, NULL, &blob_len);
    if (err != ESP_OK) {
        nvs_close(h);
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    if (blob_len == sizeof(dash_config_v2_t)) {     /* genuine v5 blob */
        size_t len = sizeof(*cfg);
        err = nvs_get_blob(h, CFG_V2_KEY, cfg, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(*cfg) && cfg_v5_is_valid(cfg)) {
            storage_cfg_v2_normalize(cfg);
            return true;
        }
        if (err == ESP_OK) ESP_LOGW(TAG, "Ignoring invalid cfg_v2 v5 blob");
        storage_cfg_v2_defaults(cfg);
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    if (blob_len == sizeof(dash_config_v4_t)) {
        /* Pre-v5 blob. sizeof(v3) == sizeof(v4): both arrive here, discriminated
           by version. Zero the v5 struct first so the trailing quiet arrays (and
           their padding) start clean before we overlay the L4 prefix — the
           migration leaves quiet hours disabled. */
        memset(cfg, 0, sizeof(*cfg));
        size_t len = blob_len;
        err = nvs_get_blob(h, CFG_V2_KEY, cfg, &len);
        nvs_close(h);
        if (err == ESP_OK && len == blob_len) {
            if (cfg_v4_is_valid(cfg)) {            /* v4 blob -> migrate to v5 */
                ESP_LOGW(TAG, "cfg_v2 v4: migrating to v5");
                cfg->version = DASH_CFG_V2_VERSION;
                esp_err_t save_err = storage_save_v2(cfg);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG, "v4→v5 migration save failed (err=0x%x); "
                                  "using in-memory cfg", save_err);
                    storage_cfg_v2_normalize(cfg);
                }
                return true;
            }
            if (cfg_v3_is_valid(cfg)) {            /* v3 blob -> migrate to v5 */
                ESP_LOGW(TAG, "cfg_v2 v3: migrating to v5");
                cfg->version = DASH_CFG_V2_VERSION;
                /* Overwrite the old v3 tail-padding byte with the default. */
                cfg->max_partials = DASH_MAX_PARTIALS_DEFAULT;
                esp_err_t save_err = storage_save_v2(cfg);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG, "v3→v5 migration save failed (err=0x%x); "
                                  "using in-memory cfg", save_err);
                    storage_cfg_v2_normalize(cfg);
                }
                return true;
            }
        }
        if (err == ESP_OK) ESP_LOGW(TAG, "Ignoring invalid cfg_v2 v3/v4 blob");
        storage_cfg_v2_defaults(cfg);
        storage_cfg_v2_normalize(cfg);
        return false;
    }

    if (blob_len == sizeof(dash_config_v2_legacy_t)) {
        /* Re-zero the v5 struct so the trailing fields (panel_variant,
           max_partials, quiet arrays) start clean before we overlay the legacy
           blob's bytes onto the prefix. */
        memset(cfg, 0, sizeof(*cfg));
        size_t len = blob_len;
        err = nvs_get_blob(h, CFG_V2_KEY, cfg, &len);
        nvs_close(h);
        if (err != ESP_OK || len != blob_len || !cfg_v2_legacy_is_valid(cfg)) {
            if (err == ESP_OK) ESP_LOGW(TAG, "Ignoring invalid legacy cfg_v2 blob");
            storage_cfg_v2_defaults(cfg);
            storage_cfg_v2_normalize(cfg);
            return false;
        }
        ESP_LOGW(TAG, "cfg_v2_legacy: migrating to v5");
        cfg->version = DASH_CFG_V2_VERSION;
        /* Legacy v2 blobs carry none of the trailing fields. Seed the SKU
           default panel variant (so an upgraded BW-SKU device resolves to BW,
           not a hardcoded BWR) and the default partial cap; quiet arrays stay
           zeroed (disabled). */
        cfg->panel_variant = storage_default_panel_variant();
        cfg->max_partials = DASH_MAX_PARTIALS_DEFAULT;
        cfg->max_wifi_networks = MAX_WIFI_NETWORKS;
        cfg->max_apis_per_network = MAX_APIS_PER_NETWORK;
        /* Persist the migrated config so subsequent boots take the fast
           sizeof(v5) path. A transient NVS error is non-fatal: log and
           continue with the in-memory migrated cfg — the next boot retries
           the migration rather than dropping the user's WiFi/API entries. */
        esp_err_t save_err = storage_save_v2(cfg);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "v2→v5 migration save failed (err=0x%x); using in-memory cfg",
                     save_err);
            storage_cfg_v2_normalize(cfg);
        }
        return true;
    }

    nvs_close(h);
    ESP_LOGW(TAG, "Unknown cfg_v2 blob length %u; resetting to defaults",
             (unsigned)blob_len);
    storage_cfg_v2_defaults(cfg);
    storage_cfg_v2_normalize(cfg);
    return false;
}

esp_err_t storage_save_v2(dash_config_v2_t *cfg)
{
    /* Normalize and update header fields in place. The previous version made
     * a 7 KiB stack copy to keep the API const-correct, but that doubled the
     * call's stack footprint and pushed the main task past the WDT. */
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
    cfg->write_counter++;
    cfg->crc32 = cfg_crc(cfg);
    err = nvs_set_blob(h, CFG_V2_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
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
 * part of the cfg_v2 blob — so the portal can change the regulatory domain
 * without churning or risking the ~7 KB config blob, mirroring how the SoftAP
 * password is persisted. Accepts the two world-safe "01" sentinel or a
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
