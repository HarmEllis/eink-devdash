#include "wifi_prov.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include <string.h>

static const char *TAG = "wifi_prov";

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    dash_config_t *cfg = (dash_config_t *)arg;

    if (base == WIFI_PROV_EVENT && id == WIFI_PROV_CRED_SUCCESS) {
        ESP_LOGI(TAG, "Provisioning successful");
    } else if (base == WIFI_PROV_EVENT && id == WIFI_PROV_END) {
        wifi_prov_mgr_deinit();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        cfg->provisioned = true;
    }
}

void wifi_prov_run(dash_config_t *cfg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT,
        ESP_EVENT_ANY_ID, event_handler, cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP, event_handler, cfg));

    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_cfg));
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1, NULL, "devdash-prov", NULL));

    wifi_prov_mgr_wait();
    wifi_prov_mgr_deinit();
}
