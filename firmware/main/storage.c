#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "storage";

void storage_init(void)
{
    /* nvs_flash_init() is called in app_main before storage_init */
}

void storage_load(dash_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "No saved config");
        return;
    }

    size_t len = sizeof(cfg->api_url);
    nvs_get_str(h, "api_url", cfg->api_url, &len);
    len = sizeof(cfg->device_token);
    nvs_get_str(h, "device_token", cfg->device_token, &len);

    uint8_t v = 0;
    nvs_get_u8(h, "refresh_min", &v); cfg->refresh_min = v ? v : 5;
    nvs_get_u8(h, "provisioned", &v); cfg->provisioned = (bool)v;
    nvs_get_u8(h, "last_red", &v);   cfg->last_red_state = (bool)v;
    nvs_get_u8(h, "bw_cycles", &v);  cfg->bw_fast_cycle_count = v;

    nvs_close(h);
}

void storage_save(const dash_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));

    nvs_set_str(h, "api_url", cfg->api_url);
    nvs_set_str(h, "device_token", cfg->device_token);
    nvs_set_u8(h, "refresh_min", cfg->refresh_min);
    nvs_set_u8(h, "provisioned", (uint8_t)cfg->provisioned);
    nvs_set_u8(h, "last_red", (uint8_t)cfg->last_red_state);
    nvs_set_u8(h, "bw_cycles", cfg->bw_fast_cycle_count);

    nvs_commit(h);
    nvs_close(h);
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
