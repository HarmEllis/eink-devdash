#include "improv.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_vfs_dev.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "storage.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "improv";

#define IMPROV_BUF_SIZE   256
#define IMPROV_PACKET_SIZE 280

/* Improv Serial protocol — https://www.improv-wifi.com/serial/ */
#define IMPROV_HEADER     "IMPROV"
#define IMPROV_VERSION    1

#define TYPE_CURRENT_STATE  0x01
#define TYPE_ERROR_STATE    0x02
#define TYPE_RPC_COMMAND    0x03
#define TYPE_RPC_RESULT     0x04

#define CMD_WIFI_SETTINGS   0x01
#define CMD_IDENTIFY        0x02
#define CMD_GET_DEVICE_INFO 0x03
#define CMD_LIST_CONFIG       0x40
#define CMD_SET_NETWORK       0x41
#define CMD_DELETE_NETWORK    0x42
#define CMD_REORDER_NETWORKS  0x43
#define CMD_REORDER_APIS      0x44
#define CMD_REPROVISION_WIFI  0x45

#define STATE_READY         0x02
#define STATE_PROVISIONING  0x03
#define STATE_PROVISIONED   0x04

#define ERROR_NONE              0x00
#define ERROR_INVALID_RPC       0x01
#define ERROR_UNABLE_TO_CONNECT 0x03

static TaskHandle_t s_task = NULL;
static int s_uart_fd = -1;
static EventGroupHandle_t s_done_events = NULL;
static EventBits_t s_done_bit = 0;
static volatile bool s_run = false;

/* Low-level UART I/O via VFS file descriptor. We deliberately avoid
 * uart_driver_install() because that would take over UART0 and break the
 * bootloader auto-reset sequence used by esptool / the web flasher. */

static int uart_read_one(uint8_t *byte, int timeout_ms)
{
    int ret = read(s_uart_fd, byte, 1);
    if (ret == 1) return 1;
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    ret = read(s_uart_fd, byte, 1);
    return (ret == 1) ? 1 : 0;
}

static int uart_read_n(uint8_t *buf, int n, int timeout_ms)
{
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (total < n) {
        int ret = read(s_uart_fd, buf + total, n - total);
        if (ret > 0) {
            total += ret;
        } else {
            if (xTaskGetTickCount() >= deadline) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return total;
}

static void uart_write_all(const uint8_t *data, int len)
{
    int written = 0;
    while (written < len) {
        int ret = write(s_uart_fd, data + written, len - written);
        if (ret > 0) {
            written += ret;
            continue;
        }
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        ESP_LOGW(TAG, "UART write failed at %d/%d (errno=%d)", written, len, errno);
        break;
    }
}

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t pkt[IMPROV_PACKET_SIZE];
    int pos = 0;
    if ((int)len + 10 > IMPROV_PACKET_SIZE) {
        ESP_LOGW(TAG, "Improv packet too large: %u", len);
        return;
    }
    memcpy(pkt, IMPROV_HEADER, 6);
    pos = 6;
    pkt[pos++] = IMPROV_VERSION;
    pkt[pos++] = type;
    pkt[pos++] = len;
    if (len > 0 && data) {
        memcpy(pkt + pos, data, len);
        pos += len;
    }
    uint8_t checksum = 0;
    for (int i = 6; i < pos; i++) checksum += pkt[i];
    pkt[pos++] = checksum;
    uart_write_all(pkt, pos);
}

static void send_state(uint8_t state)
{
    send_packet(TYPE_CURRENT_STATE, &state, 1);
}

static void send_error(uint8_t error)
{
    send_packet(TYPE_ERROR_STATE, &error, 1);
}

static void send_rpc_result(uint8_t command, const char *url)
{
    uint8_t data[128];
    int pos = 0;
    data[pos++] = command;
    if (url && url[0]) {
        uint8_t url_len = (uint8_t)strlen(url);
        data[pos++] = 1 + url_len;
        data[pos++] = 1;
        data[pos++] = url_len;
        memcpy(data + pos, url, url_len);
        pos += url_len;
    } else {
        data[pos++] = 0;
    }
    send_packet(TYPE_RPC_RESULT, data, pos);
}

static void send_rpc_json(uint8_t command, cJSON *payload)
{
    char *json = cJSON_PrintUnformatted(payload);
    if (!json) {
        send_error(ERROR_INVALID_RPC);
        return;
    }
    size_t json_len = strlen(json);
    if (json_len > 252) {
        ESP_LOGW(TAG, "RPC result for 0x%02X is too large (%u bytes)",
                 command, (unsigned)json_len);
        free(json);
        send_error(ERROR_INVALID_RPC);
        return;
    }

    uint8_t data[IMPROV_BUF_SIZE];
    data[0] = command;
    data[1] = (uint8_t)json_len;
    memcpy(data + 2, json, json_len);
    send_packet(TYPE_RPC_RESULT, data, (uint8_t)(json_len + 2));
    free(json);
}

static bool payload_proto_ok(cJSON *root)
{
    cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "proto_version");
    return proto && cJSON_IsNumber(proto) && proto->valueint == DASH_CFG_V2_VERSION;
}

static cJSON *parse_json_payload(const uint8_t *data, uint8_t len)
{
    if (!data || len == 0) return NULL;
    char json[IMPROV_BUF_SIZE] = {0};
    memcpy(json, data, len);
    return cJSON_Parse(json);
}

static cJSON *json_ack(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto_version", DASH_CFG_V2_VERSION);
    cJSON_AddBoolToObject(root, "ok", true);
    return root;
}

static void restart_after_ack(void)
{
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void handle_list_config(const uint8_t *data, uint8_t len)
{
    cJSON *req = parse_json_payload(data, len);
    if (!req || !payload_proto_ok(req)) {
        cJSON_Delete(req);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON_Delete(req);

    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto_version", DASH_CFG_V2_VERSION);
    cJSON_AddNumberToObject(root, "refresh_min", cfg.refresh_min);
    cJSON_AddNumberToObject(root, "last_success_network_idx",
                            cfg.last_success_network_idx);
    cJSON_AddNumberToObject(root, "last_success_api_idx",
                            cfg.last_success_api_idx);
    cJSON *nets = cJSON_AddArrayToObject(root, "networks");
    for (uint8_t i = 0; i < cfg.network_count; i++) {
        const dash_wifi_profile_t *net = &cfg.networks[i];
        cJSON *jn = cJSON_CreateObject();
        cJSON_AddNumberToObject(jn, "id", net->id);
        cJSON_AddBoolToObject(jn, "enabled", net->enabled);
        cJSON_AddStringToObject(jn, "ssid", net->ssid);
        cJSON *apis = cJSON_AddArrayToObject(jn, "apis");
        for (uint8_t j = 0; j < net->api_count; j++) {
            const dash_api_profile_t *api = &net->apis[j];
            char masked[DASH_DEVICE_TOKEN_MAX] = {0};
            storage_mask_token(api->device_token, masked, sizeof(masked));
            cJSON *ja = cJSON_CreateObject();
            cJSON_AddNumberToObject(ja, "id", api->id);
            cJSON_AddBoolToObject(ja, "enabled", api->enabled);
            cJSON_AddStringToObject(ja, "api_url", api->api_url);
            cJSON_AddStringToObject(ja, "device_token", masked);
            cJSON_AddItemToArray(apis, ja);
        }
        cJSON_AddItemToArray(nets, jn);
    }
    send_rpc_json(CMD_LIST_CONFIG, root);
    cJSON_Delete(root);
}

static int find_network_by_id_or_ssid(const dash_config_v2_t *cfg,
                                      uint32_t id, const char *ssid)
{
    for (uint8_t i = 0; i < cfg->network_count; i++) {
        if (id != 0 && cfg->networks[i].id == id) return i;
        if (ssid && ssid[0] != '\0' && strcmp(cfg->networks[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void handle_set_network(const uint8_t *data, uint8_t len)
{
    cJSON *root = parse_json_payload(data, len);
    if (!root || !payload_proto_ok(root)) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *apis = cJSON_GetObjectItemCaseSensitive(root, "apis");
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(ssid) || !cJSON_IsArray(apis) ||
        cJSON_GetArraySize(apis) > MAX_APIS_PER_NETWORK) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }

    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    uint32_t id = cJSON_IsNumber(id_item) ? (uint32_t)id_item->valuedouble : 0;
    int idx = find_network_by_id_or_ssid(&cfg, id, ssid->valuestring);
    if (idx < 0) {
        if (cfg.network_count >= MAX_WIFI_NETWORKS) {
            cJSON_Delete(root);
            send_error(ERROR_INVALID_RPC);
            return;
        }
        idx = cfg.network_count++;
        cfg.networks[idx].id = storage_next_profile_id(&cfg);
        cfg.networks[idx].enabled = true;
    }

    dash_wifi_profile_t *net = &cfg.networks[idx];
    net->enabled = true;
    strncpy(net->ssid, ssid->valuestring, sizeof(net->ssid) - 1);
    if (cJSON_IsString(password) && password->valuestring[0] != '\0') {
        strncpy(net->password, password->valuestring, sizeof(net->password) - 1);
    }
    net->api_count = (uint8_t)cJSON_GetArraySize(apis);
    for (uint8_t i = 0; i < net->api_count; i++) {
        cJSON *ja = cJSON_GetArrayItem(apis, i);
        cJSON *url = cJSON_GetObjectItemCaseSensitive(ja, "api_url");
        cJSON *token = cJSON_GetObjectItemCaseSensitive(ja, "device_token");
        cJSON *api_id = cJSON_GetObjectItemCaseSensitive(ja, "id");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(ja, "enabled");
        if (!cJSON_IsString(url) || !storage_validate_api_url(url->valuestring) ||
            (token && !cJSON_IsString(token))) {
            cJSON_Delete(root);
            send_error(ERROR_INVALID_RPC);
            return;
        }
        dash_api_profile_t *api = &net->apis[i];
        api->id = cJSON_IsNumber(api_id) ? (uint32_t)api_id->valuedouble
                                         : storage_next_profile_id(&cfg);
        api->enabled = enabled ? cJSON_IsTrue(enabled) : true;
        strncpy(api->api_url, url->valuestring, sizeof(api->api_url) - 1);
        if (token && token->valuestring[0] != '\0' &&
            strncmp(token->valuestring, "****", 4) != 0) {
            strncpy(api->device_token, token->valuestring,
                    sizeof(api->device_token) - 1);
        }
    }
    storage_save_v2(&cfg);
    cJSON_Delete(root);
    cJSON *ack = json_ack();
    send_rpc_json(CMD_SET_NETWORK, ack);
    cJSON_Delete(ack);
    restart_after_ack();
}

static void handle_delete_network(const uint8_t *data, uint8_t len)
{
    cJSON *root = parse_json_payload(data, len);
    if (!root || !payload_proto_ok(root)) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsNumber(id_item)) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    int idx = find_network_by_id_or_ssid(&cfg, (uint32_t)id_item->valuedouble, NULL);
    if (idx >= 0) {
        for (uint8_t i = idx; i + 1 < cfg.network_count; i++) {
            cfg.networks[i] = cfg.networks[i + 1];
        }
        cfg.network_count--;
        storage_save_v2(&cfg);
    }
    cJSON_Delete(root);
    cJSON *ack = json_ack();
    send_rpc_json(CMD_DELETE_NETWORK, ack);
    cJSON_Delete(ack);
    restart_after_ack();
}

static void handle_reorder_networks(const uint8_t *data, uint8_t len)
{
    cJSON *root = parse_json_payload(data, len);
    cJSON *ids = root ? cJSON_GetObjectItemCaseSensitive(root, "ids") : NULL;
    if (!root || !payload_proto_ok(root) || !cJSON_IsArray(ids) ||
        cJSON_GetArraySize(ids) > MAX_WIFI_NETWORKS) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    dash_config_v2_t cfg = {0}, next = {0};
    storage_load_v2(&cfg);
    next = cfg;
    next.network_count = 0;
    for (int i = 0; i < cJSON_GetArraySize(ids); i++) {
        cJSON *id_item = cJSON_GetArrayItem(ids, i);
        int idx = cJSON_IsNumber(id_item)
            ? find_network_by_id_or_ssid(&cfg, (uint32_t)id_item->valuedouble, NULL)
            : -1;
        if (idx >= 0) next.networks[next.network_count++] = cfg.networks[idx];
    }
    storage_save_v2(&next);
    cJSON_Delete(root);
    cJSON *ack = json_ack();
    send_rpc_json(CMD_REORDER_NETWORKS, ack);
    cJSON_Delete(ack);
    restart_after_ack();
}

static void handle_reorder_apis(const uint8_t *data, uint8_t len)
{
    cJSON *root = parse_json_payload(data, len);
    cJSON *net_id = root ? cJSON_GetObjectItemCaseSensitive(root, "network_id") : NULL;
    cJSON *ids = root ? cJSON_GetObjectItemCaseSensitive(root, "ids") : NULL;
    if (!root || !payload_proto_ok(root) || !cJSON_IsNumber(net_id) ||
        !cJSON_IsArray(ids) || cJSON_GetArraySize(ids) > MAX_APIS_PER_NETWORK) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    int nidx = find_network_by_id_or_ssid(&cfg, (uint32_t)net_id->valuedouble, NULL);
    if (nidx >= 0) {
        dash_wifi_profile_t *net = &cfg.networks[nidx];
        dash_api_profile_t old[MAX_APIS_PER_NETWORK] = {0};
        uint8_t old_count = net->api_count;
        memcpy(old, net->apis, sizeof(old));
        net->api_count = 0;
        for (int i = 0; i < cJSON_GetArraySize(ids); i++) {
            cJSON *id_item = cJSON_GetArrayItem(ids, i);
            if (!cJSON_IsNumber(id_item)) continue;
            for (uint8_t j = 0; j < old_count; j++) {
                if (old[j].id == (uint32_t)id_item->valuedouble) {
                    net->apis[net->api_count++] = old[j];
                    break;
                }
            }
        }
        storage_save_v2(&cfg);
    }
    cJSON_Delete(root);
    cJSON *ack = json_ack();
    send_rpc_json(CMD_REORDER_APIS, ack);
    cJSON_Delete(ack);
    restart_after_ack();
}

static void handle_reprovision_wifi(const uint8_t *data, uint8_t len)
{
    cJSON *root = parse_json_payload(data, len);
    if (!root || !payload_proto_ok(root)) {
        cJSON_Delete(root);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON_Delete(root);
    dash_config_v2_t cfg = {0};
    storage_cfg_v2_defaults(&cfg);
    storage_save_v2(&cfg);
    wifi_config_t empty = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    cJSON *ack = json_ack();
    send_rpc_json(CMD_REPROVISION_WIFI, ack);
    cJSON_Delete(ack);
    restart_after_ack();
}

/* Try to connect to the given SSID/password. Returns ESP_OK on GOT_IP. The
 * STA event handler in wifi_prov.c is already registered and will drive the
 * actual connect retries; here we just push new creds and wait. */
static esp_err_t try_connect(const char *ssid, const char *pass)
{
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) return err;

    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) return err;

    /* Poll the STA netif for an IP — keeps us decoupled from the event
     * group bits owned by wifi_prov.c. Bail out fast if improv_stop() was
     * called, so the SoftAP-prov path can deinit without racing us. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 60 && s_run; i++) {  /* ~30 s */
        if (sta) {
            esp_netif_ip_info_t ip = {0};
            if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return s_run ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

static void handle_wifi_settings(const uint8_t *data, uint8_t len)
{
    if (len < 2) { send_error(ERROR_INVALID_RPC); return; }

    int pos = 0;
    uint8_t ssid_len = data[pos++];
    if (pos + ssid_len > len) { send_error(ERROR_INVALID_RPC); return; }

    char ssid[33] = {0};
    memcpy(ssid, data + pos, ssid_len < 32 ? ssid_len : 32);
    pos += ssid_len;

    if (pos >= len) { send_error(ERROR_INVALID_RPC); return; }
    uint8_t pass_len = data[pos++];
    if (pos + pass_len > len) { send_error(ERROR_INVALID_RPC); return; }

    char pass[65] = {0};
    memcpy(pass, data + pos, pass_len < 64 ? pass_len : 64);

    ESP_LOGI(TAG, "Received WiFi credentials for SSID: %s", ssid);
    send_state(STATE_PROVISIONING);

    esp_err_t err = try_connect(ssid, pass);
    if (err == ESP_OK) {
        send_state(STATE_PROVISIONED);

        char url[64] = {0};
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {0};
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
            snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&ip.ip));
        }
        send_rpc_result(CMD_WIFI_SETTINGS, url);

        if (s_done_events) xEventGroupSetBits(s_done_events, s_done_bit);
    } else {
        ESP_LOGE(TAG, "Improv WiFi connect failed: %s", esp_err_to_name(err));
        send_error(ERROR_UNABLE_TO_CONNECT);
        send_state(STATE_READY);
    }
}

static void handle_device_info(void)
{
    uint8_t data[128];
    int pos = 0;
    data[pos++] = CMD_GET_DEVICE_INFO;

#ifdef CONFIG_IDF_TARGET_ESP32S3
    const char *chip = "ESP32-S3";
#else
    const char *chip = "ESP32";
#endif
    const char *strings[] = {
        "eink-devdash",  /* firmware name */
        "1.0.0",         /* firmware version */
        chip,            /* hardware chip */
        "devdash",       /* device name */
    };

    uint8_t total_len = 0;
    for (int i = 0; i < 4; i++) total_len += 1 + strlen(strings[i]);
    data[pos++] = total_len;

    for (int i = 0; i < 4; i++) {
        uint8_t slen = strlen(strings[i]);
        data[pos++] = slen;
        memcpy(data + pos, strings[i], slen);
        pos += slen;
    }
    send_packet(TYPE_RPC_RESULT, data, pos);
}

static void handle_rpc(const uint8_t *data, uint8_t len)
{
    /* RPC frame layout: [command, payload_len, ...payload].
     * Reject anything that does not match this exactly — otherwise a
     * malformed packet with payload_len > 0 but no payload would let
     * handlers dereference a NULL/short buffer. */
    if (len < 2) { send_error(ERROR_INVALID_RPC); return; }
    uint8_t command = data[0];
    uint8_t cmd_data_len = data[1];
    if ((uint16_t)cmd_data_len + 2u != (uint16_t)len) {
        send_error(ERROR_INVALID_RPC);
        return;
    }
    const uint8_t *cmd_data = cmd_data_len > 0 ? data + 2 : NULL;

    switch (command) {
    case CMD_WIFI_SETTINGS:
        handle_wifi_settings(cmd_data, cmd_data_len);
        break;
    case CMD_IDENTIFY:
        /* No identify hardware on this device — just ack. */
        send_rpc_result(CMD_IDENTIFY, NULL);
        break;
    case CMD_GET_DEVICE_INFO:
        handle_device_info();
        break;
    case CMD_LIST_CONFIG:
        handle_list_config(cmd_data, cmd_data_len);
        break;
    case CMD_SET_NETWORK:
        handle_set_network(cmd_data, cmd_data_len);
        break;
    case CMD_DELETE_NETWORK:
        handle_delete_network(cmd_data, cmd_data_len);
        break;
    case CMD_REORDER_NETWORKS:
        handle_reorder_networks(cmd_data, cmd_data_len);
        break;
    case CMD_REORDER_APIS:
        handle_reorder_apis(cmd_data, cmd_data_len);
        break;
    case CMD_REPROVISION_WIFI:
        handle_reprovision_wifi(cmd_data, cmd_data_len);
        break;
    default:
        ESP_LOGW(TAG, "Unknown RPC command: 0x%02X", command);
        send_error(ERROR_INVALID_RPC);
        break;
    }
}

static void improv_task(void *arg)
{
    uint8_t buf[IMPROV_BUF_SIZE];

    send_state(STATE_READY);

    while (s_run) {
        uint8_t b;
        if (uart_read_one(&b, 100) != 1) continue;
        if (b != 'I') continue;

        buf[0] = b;
        int n = uart_read_n(buf + 1, 5, 50);
        if (n != 5 || memcmp(buf, IMPROV_HEADER, 6) != 0) continue;

        n = uart_read_n(buf, 3, 50);
        if (n != 3) continue;

        uint8_t version = buf[0];
        uint8_t type = buf[1];
        uint8_t data_len = buf[2];

        if (version != IMPROV_VERSION) {
            ESP_LOGW(TAG, "Unsupported Improv version: %d", version);
            continue;
        }

        uint8_t data[IMPROV_BUF_SIZE];
        if (data_len > 0) {
            n = uart_read_n(data, data_len, 100);
            if (n != data_len) continue;
        }

        uint8_t checksum_byte;
        n = uart_read_n(&checksum_byte, 1, 50);
        if (n != 1) continue;

        uint8_t checksum = version + type + data_len;
        for (int i = 0; i < data_len; i++) checksum += data[i];
        if (checksum != checksum_byte) {
            ESP_LOGW(TAG, "Improv checksum mismatch");
            continue;
        }

        if (type == TYPE_RPC_COMMAND) {
            handle_rpc(data, data_len);
        } else {
            ESP_LOGW(TAG, "Unhandled Improv packet type: 0x%02X", type);
        }
    }

    if (s_uart_fd >= 0) {
        close(s_uart_fd);
        s_uart_fd = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t improv_start(EventGroupHandle_t done_events, EventBits_t done_bit)
{
    if (s_task) return ESP_OK;  /* already running */

    s_done_events = done_events;
    s_done_bit = done_bit;

    s_uart_fd = open("/dev/uart/0", O_RDWR | O_NONBLOCK);
    if (s_uart_fd < 0) {
        ESP_LOGE(TAG, "Failed to open /dev/uart/0: %d", errno);
        return ESP_FAIL;
    }

    s_run = true;
    BaseType_t ret = xTaskCreate(improv_task, "improv", 4096, NULL, 5, &s_task);
    if (ret != pdPASS) {
        s_run = false;
        close(s_uart_fd);
        s_uart_fd = -1;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void improv_stop(void)
{
    if (!s_task) return;
    s_run = false;
    /* Block until the task has actually finished — otherwise the caller
     * (e.g. wifi_prov.c) may deinit the WiFi stack while improv_task is
     * still inside esp_wifi_set_config()/connect(). The task self-deletes
     * after closing the UART fd; we just wait for it to clear s_task.
     * Bounded wait: ~35 s = longer than try_connect()'s 30 s budget. */
    TickType_t start = xTaskGetTickCount();
    for (int i = 0; i < 350 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
    if (s_task) {
        ESP_LOGW(TAG, "improv task did not stop in time (waited %ums)",
                 (unsigned)elapsed_ms);
    } else {
        ESP_LOGI(TAG, "improv task stopped in %ums", (unsigned)elapsed_ms);
    }
}
