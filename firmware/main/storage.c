#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "storage";

void storage_init(void)
{
    /* nvs_flash_init() is called in app_main before storage_init */
}

void storage_load(dash_config_t *cfg)
{
    /* Defaults from Kconfig — overridden by any value present in NVS. */
    strncpy(cfg->api_url, CONFIG_DEVDASH_API_URL, sizeof(cfg->api_url) - 1);
    cfg->api_url[sizeof(cfg->api_url) - 1] = '\0';
    strncpy(cfg->device_token, CONFIG_DEVDASH_DEVICE_TOKEN,
            sizeof(cfg->device_token) - 1);
    cfg->device_token[sizeof(cfg->device_token) - 1] = '\0';
    cfg->refresh_min = CONFIG_DEVDASH_REFRESH_MIN;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "No saved config, using Kconfig defaults");
        return;
    }

    size_t len = sizeof(cfg->api_url);
    nvs_get_str(h, "api_url", cfg->api_url, &len);
    len = sizeof(cfg->device_token);
    nvs_get_str(h, "device_token", cfg->device_token, &len);

    uint8_t v = 0;
    if (nvs_get_u8(h, "refresh_min", &v) == ESP_OK && v >= 3 && v <= 60)
        cfg->refresh_min = v;

    nvs_close(h);
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
