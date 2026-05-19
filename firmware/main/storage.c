#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_rom_crc.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

static const char *TAG = "storage";
static const char *CFG_V2_KEY = "cfg_v2";

_Static_assert(sizeof(dash_api_profile_t) <= 272,
               "dash_api_profile_t grew unexpectedly — recompute blob budget");
_Static_assert(sizeof(dash_wifi_profile_t) <= 1464,
               "dash_wifi_profile_t grew unexpectedly — recompute blob budget");
_Static_assert(sizeof(dash_config_v2_t) <= DASH_CFG_V2_MAX_BYTES,
               "cfg_v2 exceeds DASH_CFG_V2_MAX_BYTES — switch to per-network blobs");
/* Caps×counts budget: keeps the comfort margin obvious if caps move. */
_Static_assert((size_t)DASH_API_URL_MAX * MAX_APIS_PER_NETWORK * MAX_WIFI_NETWORKS +
               (size_t)DASH_DEVICE_TOKEN_MAX * MAX_APIS_PER_NETWORK * MAX_WIFI_NETWORKS <=
               DASH_CFG_V2_MAX_BYTES - 512,
               "URL/token caps × networks × APIs leave <512 B for headers — "
               "lower caps or switch to per-network blobs");

static uint8_t clamp_refresh(uint8_t value)
{
    if (value < 3) return 3;
    if (value > 60) return 60;
    return value;
}

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static uint32_t cfg_crc(const dash_config_v2_t *cfg)
{
    /* Piecewise CRC over the struct with the crc32 field treated as zero,
     * matching the original "copy struct, zero crc32 field, CRC the whole
     * thing" semantics — without a 7 KiB stack copy that would overflow the
     * default main task stack. */
    const uint8_t *bytes = (const uint8_t *)cfg;
    const size_t crc_off = offsetof(dash_config_v2_t, crc32);
    static const uint8_t zero_crc[sizeof(((dash_config_v2_t *)0)->crc32)] = {0};
    uint32_t crc = esp_rom_crc32_le(UINT32_MAX, bytes, crc_off);
    crc = esp_rom_crc32_le(crc, zero_crc, sizeof(zero_crc));
    crc = esp_rom_crc32_le(crc, bytes + crc_off + sizeof(zero_crc),
                           sizeof(*cfg) - crc_off - sizeof(zero_crc));
    return crc;
}

static bool cfg_v2_is_valid(const dash_config_v2_t *cfg)
{
    if (!cfg) return false;
    if (cfg->version != DASH_CFG_V2_VERSION) return false;
    if (cfg->max_wifi_networks != MAX_WIFI_NETWORKS) return false;
    if (cfg->max_apis_per_network != MAX_APIS_PER_NETWORK) return false;
    if (cfg->network_count > MAX_WIFI_NETWORKS) return false;
    if (cfg->refresh_min < 3 || cfg->refresh_min > 60) return false;
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (cfg->networks[i].api_count > MAX_APIS_PER_NETWORK) return false;
    }
    return cfg->crc32 == cfg_crc(cfg);
}

void storage_cfg_v2_defaults(dash_config_v2_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = DASH_CFG_V2_VERSION;
    cfg->max_wifi_networks = MAX_WIFI_NETWORKS;
    cfg->max_apis_per_network = MAX_APIS_PER_NETWORK;
    cfg->refresh_min = clamp_refresh(CONFIG_DEVDASH_REFRESH_MIN);
    cfg->last_success_network_idx = -1;
    cfg->last_success_api_idx = -1;
}

bool storage_validate_api_url(const char *url)
{
    if (!url || url[0] == '\0') return false;
    if (strncmp(url, "http://", 7) != 0) return false;
    size_t len = strlen(url);
    if (len >= DASH_API_URL_MAX) return false;
    const char *host = url + 7;
    if (*host == '\0') return false;
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
    cfg->refresh_min = clamp_refresh(cfg->refresh_min);
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

void storage_load_v2(dash_config_v2_t *cfg)
{
    storage_cfg_v2_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(*cfg);
        esp_err_t err = nvs_get_blob(h, CFG_V2_KEY, cfg, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(*cfg) && cfg_v2_is_valid(cfg)) {
            storage_cfg_v2_normalize(cfg);
            return;
        }
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Ignoring invalid cfg_v2 blob");
        }
    }

    /* No (valid) cfg_v2 blob yet. Leave network_count=0 so app_main runs the
     * provisioning flow. Defaults already cover refresh_min and the version
     * fields. */
    storage_cfg_v2_normalize(cfg);
}

esp_err_t storage_save_v2(dash_config_v2_t *cfg)
{
    /* Normalize and update header fields in place. The previous version made
     * a 7 KiB stack copy to keep the API const-correct, but that doubled the
     * call's stack footprint and pushed the main task past the WDT. */
    storage_cfg_v2_normalize(cfg);

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

esp_err_t storage_seed_current_sta(dash_config_v2_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    wifi_config_t sta = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK ||
        sta.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    /* Two cases that both need seeding: never provisioned (no networks at all)
     * or a placeholder networks[0] with an empty SSID — the latter can happen
     * after a stale cfg_v2 blob from earlier firmware versions. Otherwise
     * preserve whatever the user configured. */
    if (cfg->network_count > 0 && cfg->networks[0].ssid[0] != '\0') {
        return ESP_OK;
    }

    if (cfg->network_count == 0) cfg->network_count = 1;
    dash_wifi_profile_t *net = &cfg->networks[0];
    if (net->id == 0) net->id = storage_next_profile_id(cfg);
    net->enabled = true;
    copy_str(net->ssid, sizeof(net->ssid), (const char *)sta.sta.ssid);
    copy_str(net->password, sizeof(net->password),
             (const char *)sta.sta.password);
    return storage_save_v2(cfg);
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
