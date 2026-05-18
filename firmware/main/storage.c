#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_rom_crc.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "storage";
static const char *CFG_V2_KEY = "cfg_v2";

_Static_assert(sizeof(dash_config_v2_t) < 8192, "cfg_v2 must fit comfortably in NVS");

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
    dash_config_v2_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&tmp, sizeof(tmp));
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
}

void storage_load(dash_config_t *cfg)
{
    dash_config_v2_t v2 = {0};
    storage_load_v2(&v2);
    cfg->api_url[0] = '\0';
    cfg->device_token[0] = '\0';
    cfg->refresh_min = v2.refresh_min;

    if (v2.network_count > 0 && v2.networks[0].api_count > 0) {
        copy_str(cfg->api_url, sizeof(cfg->api_url), v2.networks[0].apis[0].api_url);
        copy_str(cfg->device_token, sizeof(cfg->device_token),
                 v2.networks[0].apis[0].device_token);
    }
}

static void load_legacy_values(dash_config_t *legacy)
{
    copy_str(legacy->api_url, sizeof(legacy->api_url), CONFIG_DEVDASH_API_URL);
    copy_str(legacy->device_token, sizeof(legacy->device_token),
             CONFIG_DEVDASH_DEVICE_TOKEN);
    legacy->refresh_min = clamp_refresh(CONFIG_DEVDASH_REFRESH_MIN);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(legacy->api_url);
    nvs_get_str(h, "api_url", legacy->api_url, &len);
    len = sizeof(legacy->device_token);
    nvs_get_str(h, "device_token", legacy->device_token, &len);

    uint8_t v = 0;
    if (nvs_get_u8(h, "refresh_min", &v) == ESP_OK) {
        legacy->refresh_min = clamp_refresh(v);
    }

    nvs_close(h);
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

    dash_config_t legacy = {0};
    load_legacy_values(&legacy);
    cfg->refresh_min = legacy.refresh_min;

    wifi_config_t sta = {0};
    bool has_sta = esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK &&
                   sta.sta.ssid[0] != '\0';
    bool has_api = storage_validate_api_url(legacy.api_url);

    if (has_sta || has_api) {
        cfg->network_count = 1;
        dash_wifi_profile_t *net = &cfg->networks[0];
        net->id = 1;
        net->enabled = true;
        if (has_sta) {
            copy_str(net->ssid, sizeof(net->ssid), (const char *)sta.sta.ssid);
            copy_str(net->password, sizeof(net->password),
                     (const char *)sta.sta.password);
        }
        if (has_api) {
            net->api_count = 1;
            net->apis[0].id = 2;
            net->apis[0].enabled = true;
            copy_str(net->apis[0].api_url, sizeof(net->apis[0].api_url),
                     legacy.api_url);
            copy_str(net->apis[0].device_token,
                     sizeof(net->apis[0].device_token), legacy.device_token);
        }
    }

    storage_cfg_v2_normalize(cfg);
    if (storage_save_v2(cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Migrated legacy config to cfg_v2");
    }
}

void storage_save(const dash_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));

    nvs_set_str(h, "api_url", cfg->api_url);
    nvs_set_str(h, "device_token", cfg->device_token);
    nvs_set_u8(h, "refresh_min", cfg->refresh_min);

    nvs_commit(h);
    nvs_close(h);

    dash_config_v2_t v2 = {0};
    storage_load_v2(&v2);
    if (v2.network_count == 0) {
        v2.network_count = 1;
        v2.networks[0].id = 1;
        v2.networks[0].enabled = true;
    }
    if (v2.networks[0].api_count == 0) {
        v2.networks[0].api_count = 1;
        v2.networks[0].apis[0].id = storage_next_profile_id(&v2);
        v2.networks[0].apis[0].enabled = true;
    }
    copy_str(v2.networks[0].apis[0].api_url,
             sizeof(v2.networks[0].apis[0].api_url), cfg->api_url);
    copy_str(v2.networks[0].apis[0].device_token,
             sizeof(v2.networks[0].apis[0].device_token), cfg->device_token);
    v2.refresh_min = cfg->refresh_min;
    storage_save_v2(&v2);
}

esp_err_t storage_save_v2(const dash_config_v2_t *cfg)
{
    dash_config_v2_t copy = *cfg;
    storage_cfg_v2_normalize(&copy);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    copy.write_counter++;
    copy.crc32 = cfg_crc(&copy);
    err = nvs_set_blob(h, CFG_V2_KEY, &copy, sizeof(copy));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_seed_current_sta_if_empty(dash_config_v2_t *cfg)
{
    if (!cfg || cfg->network_count > 0) return ESP_OK;

    wifi_config_t sta = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK ||
        sta.sta.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    dash_config_t legacy = {0};
    load_legacy_values(&legacy);

    cfg->network_count = 1;
    cfg->networks[0].id = storage_next_profile_id(cfg);
    cfg->networks[0].enabled = true;
    copy_str(cfg->networks[0].ssid, sizeof(cfg->networks[0].ssid),
             (const char *)sta.sta.ssid);
    copy_str(cfg->networks[0].password, sizeof(cfg->networks[0].password),
             (const char *)sta.sta.password);
    if (storage_validate_api_url(legacy.api_url)) {
        cfg->networks[0].api_count = 1;
        cfg->networks[0].apis[0].id = storage_next_profile_id(cfg);
        cfg->networks[0].apis[0].enabled = true;
        copy_str(cfg->networks[0].apis[0].api_url,
                 sizeof(cfg->networks[0].apis[0].api_url), legacy.api_url);
        copy_str(cfg->networks[0].apis[0].device_token,
                 sizeof(cfg->networks[0].apis[0].device_token),
                 legacy.device_token);
    }
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
