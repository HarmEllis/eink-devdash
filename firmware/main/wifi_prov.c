#include "wifi_prov.h"
#include "improv.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "wifi_net";

static EventGroupHandle_t s_wifi_events;
#define BIT_PROV_DONE     BIT2
#define PORTAL_BODY_MAX   768

static char s_portal_ssid[32];
static char s_portal_password[16];

typedef struct {
    char ssid[DASH_SSID_MAX + 1];
    char password[DASH_WIFI_PASSWORD_MAX + 1];
    char api_url[DASH_API_URL_MAX];
    char device_token[DASH_DEVICE_TOKEN_MAX];
} portal_form_t;

/* Connection is driven explicitly by wifi_roam_connect (esp_wifi_connect on
 * the SSID we pick after scanning) and by Improv when the optional USB
 * provisioning path is used.
 * We intentionally do NOT auto-connect on STA_START or auto-retry on
 * STA_DISCONNECTED here: doing so kicks off a connect with whatever credentials
 * are still in WiFi NVS and blocks the scan path with
 * "STA is connecting, scan are not allowed!". */

esp_err_t wifi_net_init(void)
{
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return ESP_OK;
}

bool wifi_net_is_provisioned(void)
{
    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    return cfg.network_count > 0 && cfg.networks[0].ssid[0] != '\0';
}

/* Build a per-device AP password and SSID derived from the factory MAC, so two
 * devices in range cannot impersonate each other with a published default. */
static void derive_device_ids(char *ssid, size_t ssid_sz,
                              char *pop, size_t pop_sz)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_sz, "devdash-%02X%02X", mac[4], mac[5]);
    /* 12 hex chars = 48 bits of MAC-derived entropy; stable per device and
     * printed on the e-ink display during provisioning. */
    snprintf(pop, pop_sz, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wifi_net_get_prov_info(char *ssid, size_t ssid_sz,
                            char *pop, size_t pop_sz)
{
    derive_device_ids(ssid, ssid_sz, pop, pop_sz);
}

static void send_portal_page(httpd_req_t *req, const char *message, bool success)
{
    char header[768];
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    snprintf(header, sizeof(header),
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>eink-devdash provisioning</title>"
        "<style>body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;"
        "max-width:34rem;margin:2rem auto;padding:0 1rem;color:#172126}"
        "label{display:grid;gap:.25rem;margin:.9rem 0;font-size:.9rem}"
        "input{font:inherit;padding:.65rem;border:1px solid #a8b7bd;border-radius:6px}"
        "button{font:inherit;padding:.7rem 1rem;border:0;border-radius:6px;"
        "background:#17466b;color:white}.msg{padding:.7rem;border-radius:6px;"
        "background:%s;color:%s}.hint{color:#5c6a70;font-size:.9rem}</style></head><body>",
        success ? "#e8f5ee" : "#fff4d6",
        success ? "#17633e" : "#5f4700");
    httpd_resp_sendstr_chunk(req, header);
    httpd_resp_sendstr_chunk(req, "<h1>eink-devdash provisioning</h1>");
    if (message && message[0]) {
        httpd_resp_sendstr_chunk(req, "<p class=\"msg\">");
        httpd_resp_sendstr_chunk(req, message);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req,
        "<p class=\"hint\">Connect this device to 2.4 GHz WiFi and point it at "
        "the dashboard API. API URLs must start with <code>http://</code>.</p>"
        "<form method=\"post\" action=\"/provision\">"
        "<label>WiFi SSID<input name=\"ssid\" required maxlength=\"32\" autocomplete=\"off\"></label>"
        "<label>WiFi password<input name=\"password\" type=\"password\" maxlength=\"64\"></label>"
        "<label>API URL<input name=\"api_url\" required maxlength=\"191\" value=\"http://\" inputmode=\"url\"></label>"
        "<label>Device token<input name=\"device_token\" required type=\"password\" maxlength=\"63\"></label>"
        "<button type=\"submit\">Save and reboot</button></form>"
        "<p class=\"hint\">After saving, reconnect your computer or phone to "
        "your normal WiFi. The device will restart and fetch the dashboard.</p>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "Connected to %s. AP password: %s.",
             s_portal_ssid, s_portal_password);
    send_portal_page(req, msg, false);
    return ESP_OK;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void form_decode(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_sz; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static void parse_form_field(portal_form_t *form, const char *key, size_t key_len,
                             const char *value, size_t value_len)
{
    if (key_len == 4 && strncmp(key, "ssid", key_len) == 0) {
        form_decode(form->ssid, sizeof(form->ssid), value, value_len);
    } else if (key_len == 8 && strncmp(key, "password", key_len) == 0) {
        form_decode(form->password, sizeof(form->password), value, value_len);
    } else if (key_len == 7 && strncmp(key, "api_url", key_len) == 0) {
        form_decode(form->api_url, sizeof(form->api_url), value, value_len);
    } else if (key_len == 12 && strncmp(key, "device_token", key_len) == 0) {
        form_decode(form->device_token, sizeof(form->device_token), value, value_len);
    }
}

static void parse_form_body(char *body, portal_form_t *form)
{
    char *p = body;
    while (*p) {
        char *key = p;
        char *eq = strchr(key, '=');
        char *amp = strchr(key, '&');
        if (!eq || (amp && amp < eq)) {
            if (!amp) break;
            p = amp + 1;
            continue;
        }
        char *value = eq + 1;
        char *end = amp ? amp : body + strlen(body);
        parse_form_field(form, key, (size_t)(eq - key), value, (size_t)(end - value));
        if (!amp) break;
        p = amp + 1;
    }
}

static esp_err_t save_portal_config(const portal_form_t *form)
{
    if (form->ssid[0] == '\0' || form->device_token[0] == '\0' ||
        !storage_validate_api_url(form->api_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);

    uint32_t network_id = cfg.network_count > 0 && cfg.networks[0].id != 0
        ? cfg.networks[0].id : 1;
    uint32_t api_id = cfg.network_count > 0 && cfg.networks[0].api_count > 0 &&
                      cfg.networks[0].apis[0].id != 0
        ? cfg.networks[0].apis[0].id : network_id + 1;

    storage_cfg_v2_defaults(&cfg);
    cfg.network_count = 1;
    dash_wifi_profile_t *net = &cfg.networks[0];
    net->id = network_id;
    net->enabled = true;
    strncpy(net->ssid, form->ssid, sizeof(net->ssid) - 1);
    strncpy(net->password, form->password, sizeof(net->password) - 1);
    net->api_count = 1;

    dash_api_profile_t *api = &net->apis[0];
    api->id = api_id;
    api->enabled = true;
    strncpy(api->api_url, form->api_url, sizeof(api->api_url) - 1);
    strncpy(api->device_token, form->device_token, sizeof(api->device_token) - 1);

    return storage_save_v2(&cfg);
}

static esp_err_t portal_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PORTAL_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        send_portal_page(req, "Provisioning form payload is too large.", false);
        return ESP_OK;
    }

    char body[PORTAL_BODY_MAX + 1] = {0};
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            send_portal_page(req, "Could not read the submitted form.", false);
            return ESP_OK;
        }
        received += ret;
    }
    body[received] = '\0';

    portal_form_t form = {0};
    parse_form_body(body, &form);
    esp_err_t err = save_portal_config(&form);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        send_portal_page(req,
            "Check the fields: SSID and token are required, and API URL must start with http://.",
            false);
        return ESP_OK;
    }

    send_portal_page(req,
        "Saved. The device will restart, join WiFi, fetch the API, and render the dashboard.",
        true);
    xEventGroupSetBits(s_wifi_events, BIT_PROV_DONE);
    return ESP_OK;
}

static esp_err_t start_portal_server(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(server, &config);
    if (err != ESP_OK) return err;

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_get_handler,
    };
    httpd_uri_t provision = {
        .uri = "/provision",
        .method = HTTP_POST,
        .handler = portal_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(*server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(*server, &provision));
    return ESP_OK;
}

static esp_err_t run_provisioning_window(void)
{
    xEventGroupClearBits(s_wifi_events, BIT_PROV_DONE);

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_FAIL;

    derive_device_ids(s_portal_ssid, sizeof(s_portal_ssid),
                      s_portal_password, sizeof(s_portal_password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_portal_ssid,
            sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, s_portal_password,
            sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = strlen(s_portal_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(start_portal_server(&server));

    ESP_LOGI(TAG, "Starting SoftAP HTTP provisioning portal: ssid=%s password=%s url=http://192.168.4.1",
             s_portal_ssid, s_portal_password);

    /* Improv Serial runs in parallel as a USB recovery/editor path. The
     * first-time path is the HTTP portal on the SoftAP above, which avoids
     * ESP32-S3 USB-Serial-JTAG reset/re-enumeration behavior. */
    improv_start(s_wifi_events, BIT_PROV_DONE);

    /* Wait up to the configured timeout for provisioning to end. */
    TickType_t ticks = CONFIG_DEVDASH_PROV_TIMEOUT_S > 0
        ? pdMS_TO_TICKS(CONFIG_DEVDASH_PROV_TIMEOUT_S * 1000)
        : portMAX_DELAY;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, BIT_PROV_DONE,
                                           pdTRUE, pdFALSE, ticks);
    improv_stop();
    if (server) httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
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

void wifi_net_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}
