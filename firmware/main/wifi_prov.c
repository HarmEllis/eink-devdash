#include "wifi_prov.h"
#include "improv.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_net";

static EventGroupHandle_t s_wifi_events;
#define BIT_GOT_IP        BIT0
#define BIT_DISCONNECTED  BIT1
#define BIT_PROV_DONE     BIT2

static int s_connect_retries = 0;
#define MAX_CONNECT_RETRIES 5

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connect_retries++ < MAX_CONNECT_RETRIES) {
            ESP_LOGW(TAG, "Disconnected, retrying (%d)", s_connect_retries);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, BIT_DISCONNECTED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connect_retries = 0;
        xEventGroupSetBits(s_wifi_events, BIT_GOT_IP);
    } else if (base == WIFI_PROV_EVENT) {
        switch (id) {
            case WIFI_PROV_CRED_RECV:
                ESP_LOGI(TAG, "Received WiFi credentials");
                break;
            case WIFI_PROV_CRED_FAIL:
                ESP_LOGE(TAG, "Provisioning failed");
                break;
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                xEventGroupSetBits(s_wifi_events, BIT_PROV_DONE);
                break;
            default:
                break;
        }
    }
}

esp_err_t wifi_net_init(void)
{
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT,
        ESP_EVENT_ANY_ID, wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return ESP_OK;
}

bool wifi_net_is_provisioned(void)
{
    wifi_config_t wcfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) != ESP_OK) return false;
    return wcfg.sta.ssid[0] != '\0';
}

/* Build a per-device PoP and SSID derived from the factory MAC, so two
 * devices in range cannot impersonate each other with a published default. */
static void derive_device_ids(char *ssid, size_t ssid_sz,
                              char *pop, size_t pop_sz)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_sz, "devdash-%02X%02X", mac[4], mac[5]);
    /* 12 hex chars = 48 bits of MAC-derived entropy; not a secret but unique
     * per device and printed on the e-ink display during provisioning. */
    snprintf(pop, pop_sz, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wifi_net_get_prov_info(char *ssid, size_t ssid_sz,
                            char *pop, size_t pop_sz)
{
    derive_device_ids(ssid, ssid_sz, pop, pop_sz);
}

static esp_err_t run_provisioning_window(void)
{
    /* Need a temporary AP netif while provisioning. */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_cfg));

    char ssid[32], pop[16];
    derive_device_ids(ssid, sizeof(ssid), pop, sizeof(pop));

    ESP_LOGI(TAG, "Starting SoftAP provisioning portal: ssid=%s pop=%s", ssid, pop);
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1, pop, ssid, NULL));

    /* Improv Serial runs in parallel: user can provision via SoftAP+app OR
     * via the web flasher over USB. Whichever finishes first wins. */
    improv_start(s_wifi_events, BIT_PROV_DONE);

    /* Wait up to the configured timeout for provisioning to end. */
    TickType_t ticks = CONFIG_DEVDASH_PROV_TIMEOUT_S > 0
        ? pdMS_TO_TICKS(CONFIG_DEVDASH_PROV_TIMEOUT_S * 1000)
        : portMAX_DELAY;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, BIT_PROV_DONE,
                                           pdTRUE, pdFALSE, ticks);
    improv_stop();
    wifi_prov_mgr_deinit();
    if (ap_netif) esp_netif_destroy_default_wifi(ap_netif);

    return (bits & BIT_PROV_DONE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_net_open_config_window(void)
{
    return run_provisioning_window();
}

esp_err_t wifi_net_provision_if_needed(void)
{
    if (wifi_net_is_provisioned()) return ESP_OK;
    return run_provisioning_window();
}

esp_err_t wifi_net_forget(void)
{
    /* Clear stored STA credentials so the next call to
     * wifi_net_provision_if_needed() starts a fresh provisioning round. */
    wifi_config_t empty = {0};
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_net_forget: esp_wifi_set_config failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_net_connect(void)
{
    s_connect_retries = 0;
    xEventGroupClearBits(s_wifi_events, BIT_GOT_IP | BIT_DISCONNECTED);

    /* Make sure mode is STA and start the driver. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_NOT_STOPPED) err = ESP_OK;
    ESP_ERROR_CHECK(err);

    TickType_t ticks = pdMS_TO_TICKS(CONFIG_DEVDASH_WIFI_CONNECT_TIMEOUT_S * 1000);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           BIT_GOT_IP | BIT_DISCONNECTED,
                                           pdFALSE, pdFALSE, ticks);
    if (bits & BIT_GOT_IP) return ESP_OK;
    ESP_LOGE(TAG, "WiFi connect timeout/failure");
    return ESP_FAIL;
}

void wifi_net_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}
