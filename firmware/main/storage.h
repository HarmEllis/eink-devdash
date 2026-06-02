#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "eink_weact29.h"

#define NVS_NAMESPACE "devdash"
#define NVS_SCHEMA_VERSION 3

#define DASH_CFG_V2_VERSION 3
#define MAX_WIFI_NETWORKS 5
#define MAX_APIS_PER_NETWORK 5
#define DASH_SSID_MAX 32
#define DASH_WIFI_PASSWORD_MAX 64
#define DASH_API_URL_MAX 192
#define DASH_DEVICE_TOKEN_MAX 64

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
} dash_config_v2_t;

void storage_init(void);
/* Returns true when a v2 or v3 blob was successfully loaded from NVS,
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
