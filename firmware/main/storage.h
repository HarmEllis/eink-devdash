#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define NVS_NAMESPACE "devdash"
#define NVS_SCHEMA_VERSION 2

#define DASH_CFG_V2_VERSION 2
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
    char api_url[DASH_API_URL_MAX];
    char device_token[DASH_DEVICE_TOKEN_MAX];
    uint8_t refresh_min;   /* 3–60 */
} dash_config_t;

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
} dash_config_v2_t;

void storage_init(void);
void storage_load(dash_config_t *cfg);
void storage_save(const dash_config_t *cfg);
void storage_load_v2(dash_config_v2_t *cfg);
esp_err_t storage_save_v2(const dash_config_v2_t *cfg);
esp_err_t storage_seed_current_sta_if_empty(dash_config_v2_t *cfg);
void storage_erase(void);

void storage_cfg_v2_defaults(dash_config_v2_t *cfg);
void storage_cfg_v2_normalize(dash_config_v2_t *cfg);
bool storage_validate_api_url(const char *url);
uint32_t storage_next_profile_id(const dash_config_v2_t *cfg);
void storage_mask_token(const char *token, char *out, size_t out_sz);
