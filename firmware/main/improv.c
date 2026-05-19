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
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif
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
#define DASH_RPC_PROTO_VERSION 3
#define RPC_FIELD_CHUNK_SIZE 120

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
#define CMD_LIST_CONFIG_NETWORK 0x46
#define CMD_LIST_CONFIG_API     0x47
#define CMD_LIST_CONFIG_FIELD   0x48
#define CMD_SET_NETWORK_BEGIN   0x49
#define CMD_SET_NETWORK_API     0x4A
#define CMD_SET_NETWORK_FIELD   0x4B
#define CMD_SET_NETWORK_COMMIT  0x4C

#define STATE_READY         0x02
#define STATE_PROVISIONING  0x03
#define STATE_PROVISIONED   0x04

#define ERROR_NONE              0x00
#define ERROR_INVALID_RPC       0x01
#define ERROR_UNABLE_TO_CONNECT 0x03

static TaskHandle_t s_task = NULL;
static int s_uart_fd = -1;
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
static bool s_usb_serial_jtag_active = false;
#endif
static EventGroupHandle_t s_done_events = NULL;
static EventBits_t s_done_bit = 0;
static volatile bool s_run = false;
static dash_config_v2_t s_edit_cfg;
static dash_api_profile_t s_edit_original_apis[MAX_APIS_PER_NETWORK];
static uint8_t s_edit_original_api_count = 0;
static uint32_t s_edit_network_id = 0;
static bool s_edit_active = false;

/* Improv must use the same serial endpoint the browser opens. On ESP32-S3
 * Super Mini that is the built-in USB-Serial-JTAG CDC device, not UART0.
 *
 * We use the USB-Serial-JTAG driver API directly instead of the VFS console
 * path so Improv owns packet RX/TX while it is running. sdkconfig.defaults
 * disables the secondary USB console for this reason: log bytes on the same
 * CDC stream can corrupt Improv frames. UART0 stays as the primary console for
 * a wired debug adapter. Non-S3 targets fall back to /dev/uart/0. */

static int uart_read_one(uint8_t *byte, int timeout_ms)
{
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
    if (s_usb_serial_jtag_active) {
        int ret = usb_serial_jtag_read_bytes(byte, 1, pdMS_TO_TICKS(timeout_ms));
        return (ret == 1) ? 1 : 0;
    }
#endif
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
        int ret;
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
        if (s_usb_serial_jtag_active) {
            TickType_t now = xTaskGetTickCount();
            if (now >= deadline) break;
            ret = usb_serial_jtag_read_bytes(buf + total, n - total,
                                             deadline - now);
        } else
#endif
        {
            ret = read(s_uart_fd, buf + total, n - total);
        }
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
        int ret;
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
        if (s_usb_serial_jtag_active) {
            ret = usb_serial_jtag_write_bytes(data + written, len - written,
                                              pdMS_TO_TICKS(100));
        } else
#endif
        {
            ret = write(s_uart_fd, data + written, len - written);
        }
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
    if (json_len > 253) {
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
    return proto && cJSON_IsNumber(proto) && proto->valueint == DASH_RPC_PROTO_VERSION;
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
    cJSON_AddNumberToObject(root, "proto_version", DASH_RPC_PROTO_VERSION);
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
    cJSON_AddNumberToObject(root, "proto_version", DASH_RPC_PROTO_VERSION);
    cJSON_AddNumberToObject(root, "refresh_min", cfg.refresh_min);
    cJSON_AddNumberToObject(root, "last_success_network_idx",
                            cfg.last_success_network_idx);
    cJSON_AddNumberToObject(root, "last_success_api_idx",
                            cfg.last_success_api_idx);
    cJSON_AddNumberToObject(root, "network_count", cfg.network_count);
    cJSON_AddNumberToObject(root, "max_wifi_networks", MAX_WIFI_NETWORKS);
    cJSON_AddNumberToObject(root, "max_apis_per_network", MAX_APIS_PER_NETWORK);
    send_rpc_json(CMD_LIST_CONFIG, root);
    cJSON_Delete(root);
}

static bool get_req_index(cJSON *root, const char *key, int max, uint8_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint >= max) {
        return false;
    }
    *out = (uint8_t)item->valueint;
    return true;
}

static bool parse_proto_request(const uint8_t *data, uint8_t len, cJSON **root)
{
    *root = parse_json_payload(data, len);
    if (!*root || !payload_proto_ok(*root)) {
        cJSON_Delete(*root);
        *root = NULL;
        send_error(ERROR_INVALID_RPC);
        return false;
    }
    return true;
}

static void handle_list_config_network(const uint8_t *data, uint8_t len)
{
    cJSON *req = NULL;
    if (!parse_proto_request(data, len, &req)) return;

    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    uint8_t network_index = 0;
    if (!get_req_index(req, "network_index", cfg.network_count, &network_index)) {
        cJSON_Delete(req);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON_Delete(req);

    const dash_wifi_profile_t *net = &cfg.networks[network_index];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto_version", DASH_RPC_PROTO_VERSION);
    cJSON_AddNumberToObject(root, "id", net->id);
    cJSON_AddBoolToObject(root, "enabled", net->enabled);
    cJSON_AddStringToObject(root, "ssid", net->ssid);
    cJSON_AddNumberToObject(root, "api_count", net->api_count);
    send_rpc_json(CMD_LIST_CONFIG_NETWORK, root);
    cJSON_Delete(root);
}

static void handle_list_config_api(const uint8_t *data, uint8_t len)
{
    cJSON *req = NULL;
    if (!parse_proto_request(data, len, &req)) return;

    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    uint8_t network_index = 0, api_index = 0;
    if (!get_req_index(req, "network_index", cfg.network_count, &network_index) ||
        !get_req_index(req, "api_index", cfg.networks[network_index].api_count, &api_index)) {
        cJSON_Delete(req);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON_Delete(req);

    const dash_api_profile_t *api = &cfg.networks[network_index].apis[api_index];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto_version", DASH_RPC_PROTO_VERSION);
    cJSON_AddNumberToObject(root, "id", api->id);
    cJSON_AddBoolToObject(root, "enabled", api->enabled);
    send_rpc_json(CMD_LIST_CONFIG_API, root);
    cJSON_Delete(root);
}

static const char *api_field_value(const dash_api_profile_t *api, const char *field,
                                   char *masked, size_t masked_sz)
{
    if (strcmp(field, "api_url") == 0) return api->api_url;
    if (strcmp(field, "device_token") == 0) {
        storage_mask_token(api->device_token, masked, masked_sz);
        return masked;
    }
    return NULL;
}

static void handle_list_config_field(const uint8_t *data, uint8_t len)
{
    cJSON *req = NULL;
    if (!parse_proto_request(data, len, &req)) return;

    dash_config_v2_t cfg = {0};
    storage_load_v2(&cfg);
    uint8_t network_index = 0, api_index = 0;
    cJSON *field = cJSON_GetObjectItemCaseSensitive(req, "field");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(req, "offset");
    if (!get_req_index(req, "network_index", cfg.network_count, &network_index) ||
        !get_req_index(req, "api_index", cfg.networks[network_index].api_count, &api_index) ||
        !cJSON_IsString(field) || !cJSON_IsNumber(offset_item) ||
        offset_item->valueint < 0) {
        cJSON_Delete(req);
        send_error(ERROR_INVALID_RPC);
        return;
    }

    char masked[DASH_DEVICE_TOKEN_MAX] = {0};
    const dash_api_profile_t *api = &cfg.networks[network_index].apis[api_index];
    const char *value = api_field_value(api, field->valuestring, masked, sizeof(masked));
    if (!value) {
        cJSON_Delete(req);
        send_error(ERROR_INVALID_RPC);
        return;
    }

    size_t offset = (size_t)offset_item->valueint;
    size_t value_len = strlen(value);
    if (offset > value_len) {
        cJSON_Delete(req);
        send_error(ERROR_INVALID_RPC);
        return;
    }
    size_t chunk_len = value_len - offset;
    if (chunk_len > RPC_FIELD_CHUNK_SIZE) chunk_len = RPC_FIELD_CHUNK_SIZE;
    char chunk[RPC_FIELD_CHUNK_SIZE + 1] = {0};
    memcpy(chunk, value + offset, chunk_len);
    cJSON_Delete(req);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "proto_version", DASH_RPC_PROTO_VERSION);
    cJSON_AddStringToObject(root, "value", chunk);
    cJSON_AddBoolToObject(root, "done", offset + chunk_len >= value_len);
    send_rpc_json(CMD_LIST_CONFIG_FIELD, root);
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

static int find_api_by_id(const dash_api_profile_t *apis, uint8_t count, uint32_t id)
{
    if (id == 0) return -1;
    for (uint8_t i = 0; i < count; i++) {
        if (apis[i].id == id) return i;
    }
    return -1;
}

static dash_wifi_profile_t *edit_network(void)
{
    if (!s_edit_active || s_edit_network_id == 0) return NULL;
    int idx = find_network_by_id_or_ssid(&s_edit_cfg, s_edit_network_id, NULL);
    return idx >= 0 ? &s_edit_cfg.networks[idx] : NULL;
}

static void clear_edit_session(void)
{
    memset(&s_edit_cfg, 0, sizeof(s_edit_cfg));
    memset(s_edit_original_apis, 0, sizeof(s_edit_original_apis));
    s_edit_original_api_count = 0;
    s_edit_network_id = 0;
    s_edit_active = false;
}

static void handle_set_network_begin(const uint8_t *data, uint8_t len)
{
    cJSON *root = NULL;
    if (!parse_proto_request(data, len, &root)) return;

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *api_count_item = cJSON_GetObjectItemCaseSensitive(root, "api_count");
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!cJSON_IsString(ssid) || !cJSON_IsNumber(api_count_item) ||
        api_count_item->valueint < 0 || api_count_item->valueint > MAX_APIS_PER_NETWORK ||
        strlen(ssid->valuestring) > DASH_SSID_MAX ||
        (password && (!cJSON_IsString(password) ||
                      strlen(password->valuestring) > DASH_WIFI_PASSWORD_MAX))) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }

    storage_load_v2(&s_edit_cfg);
    uint32_t id = cJSON_IsNumber(id_item) ? (uint32_t)id_item->valuedouble : 0;
    int idx = find_network_by_id_or_ssid(&s_edit_cfg, id, ssid->valuestring);
    if (idx < 0) {
        if (s_edit_cfg.network_count >= MAX_WIFI_NETWORKS) {
            cJSON_Delete(root);
            clear_edit_session();
            send_error(ERROR_INVALID_RPC);
            return;
        }
        idx = s_edit_cfg.network_count++;
        memset(&s_edit_cfg.networks[idx], 0, sizeof(s_edit_cfg.networks[idx]));
        s_edit_cfg.networks[idx].id = storage_next_profile_id(&s_edit_cfg);
    }

    dash_wifi_profile_t *net = &s_edit_cfg.networks[idx];
    memcpy(s_edit_original_apis, net->apis, sizeof(s_edit_original_apis));
    s_edit_original_api_count = net->api_count;

    net->enabled = enabled ? cJSON_IsTrue(enabled) : true;
    strncpy(net->ssid, ssid->valuestring, sizeof(net->ssid) - 1);
    if (cJSON_IsString(password) && password->valuestring[0] != '\0') {
        strncpy(net->password, password->valuestring, sizeof(net->password) - 1);
    }
    net->api_count = (uint8_t)api_count_item->valueint;
    memset(net->apis, 0, sizeof(net->apis));

    s_edit_network_id = net->id;
    s_edit_active = true;
    cJSON_Delete(root);

    cJSON *ack = json_ack();
    cJSON_AddNumberToObject(ack, "id", s_edit_network_id);
    send_rpc_json(CMD_SET_NETWORK_BEGIN, ack);
    cJSON_Delete(ack);
}

static void handle_set_network_api(const uint8_t *data, uint8_t len)
{
    cJSON *root = NULL;
    if (!parse_proto_request(data, len, &root)) return;

    cJSON *network_id = cJSON_GetObjectItemCaseSensitive(root, "network_id");
    cJSON *api_index_item = cJSON_GetObjectItemCaseSensitive(root, "api_index");
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    dash_wifi_profile_t *net = edit_network();
    if (!net || !cJSON_IsNumber(network_id) ||
        (uint32_t)network_id->valuedouble != s_edit_network_id ||
        !cJSON_IsNumber(api_index_item) || api_index_item->valueint < 0 ||
        api_index_item->valueint >= net->api_count) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }

    uint8_t api_index = (uint8_t)api_index_item->valueint;
    uint32_t id = cJSON_IsNumber(id_item) ? (uint32_t)id_item->valuedouble : 0;
    int old_idx = find_api_by_id(s_edit_original_apis, s_edit_original_api_count, id);
    dash_api_profile_t *api = &net->apis[api_index];
    if (old_idx >= 0) {
        *api = s_edit_original_apis[old_idx];
    } else {
        memset(api, 0, sizeof(*api));
        api->id = id != 0 ? id : storage_next_profile_id(&s_edit_cfg);
    }
    api->enabled = enabled ? cJSON_IsTrue(enabled) : true;
    cJSON_Delete(root);

    cJSON *ack = json_ack();
    cJSON_AddNumberToObject(ack, "id", api->id);
    send_rpc_json(CMD_SET_NETWORK_API, ack);
    cJSON_Delete(ack);
}

static bool api_write_field(dash_api_profile_t *api, const char *field,
                            char **target, size_t *target_sz)
{
    if (strcmp(field, "api_url") == 0) {
        *target = api->api_url;
        *target_sz = sizeof(api->api_url);
        return true;
    }
    if (strcmp(field, "device_token") == 0) {
        *target = api->device_token;
        *target_sz = sizeof(api->device_token);
        return true;
    }
    return false;
}

static void handle_set_network_field(const uint8_t *data, uint8_t len)
{
    cJSON *root = NULL;
    if (!parse_proto_request(data, len, &root)) return;

    cJSON *network_id = cJSON_GetObjectItemCaseSensitive(root, "network_id");
    cJSON *api_index_item = cJSON_GetObjectItemCaseSensitive(root, "api_index");
    cJSON *field = cJSON_GetObjectItemCaseSensitive(root, "field");
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(root, "offset");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    cJSON *done = cJSON_GetObjectItemCaseSensitive(root, "done");
    dash_wifi_profile_t *net = edit_network();
    if (!net || !cJSON_IsNumber(network_id) ||
        (uint32_t)network_id->valuedouble != s_edit_network_id ||
        !cJSON_IsNumber(api_index_item) || api_index_item->valueint < 0 ||
        api_index_item->valueint >= net->api_count || !cJSON_IsString(field) ||
        !cJSON_IsNumber(offset_item) || offset_item->valueint < 0 ||
        !cJSON_IsString(value)) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }

    dash_api_profile_t *api = &net->apis[(uint8_t)api_index_item->valueint];
    char *target = NULL;
    size_t target_sz = 0;
    if (!api_write_field(api, field->valuestring, &target, &target_sz)) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }

    size_t offset = (size_t)offset_item->valueint;
    size_t current_len = strlen(target);
    size_t value_len = strlen(value->valuestring);
    if (offset == 0) {
        target[0] = '\0';
        current_len = 0;
    }
    if (offset != current_len || offset + value_len >= target_sz ||
        (strcmp(field->valuestring, "device_token") == 0 &&
         offset == 0 && strncmp(value->valuestring, "****", 4) == 0)) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }
    memcpy(target + offset, value->valuestring, value_len);
    target[offset + value_len] = '\0';

    if (cJSON_IsTrue(done) && strcmp(field->valuestring, "api_url") == 0 &&
        !storage_validate_api_url(api->api_url)) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }
    cJSON_Delete(root);

    cJSON *ack = json_ack();
    send_rpc_json(CMD_SET_NETWORK_FIELD, ack);
    cJSON_Delete(ack);
}

static void handle_set_network_commit(const uint8_t *data, uint8_t len)
{
    cJSON *root = NULL;
    if (!parse_proto_request(data, len, &root)) return;

    cJSON *network_id = cJSON_GetObjectItemCaseSensitive(root, "network_id");
    dash_wifi_profile_t *net = edit_network();
    if (!net || !cJSON_IsNumber(network_id) ||
        (uint32_t)network_id->valuedouble != s_edit_network_id) {
        cJSON_Delete(root);
        clear_edit_session();
        send_error(ERROR_INVALID_RPC);
        return;
    }
    for (uint8_t i = 0; i < net->api_count; i++) {
        if (!storage_validate_api_url(net->apis[i].api_url)) {
            cJSON_Delete(root);
            clear_edit_session();
            send_error(ERROR_INVALID_RPC);
            return;
        }
    }

    storage_save_v2(&s_edit_cfg);
    cJSON_Delete(root);
    clear_edit_session();
    cJSON *ack = json_ack();
    send_rpc_json(CMD_SET_NETWORK_COMMIT, ack);
    cJSON_Delete(ack);
    restart_after_ack();
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

    ESP_LOGI(TAG, "RPC 0x%02X (payload %u B)", command, cmd_data_len);

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
    case CMD_LIST_CONFIG_NETWORK:
        handle_list_config_network(cmd_data, cmd_data_len);
        break;
    case CMD_LIST_CONFIG_API:
        handle_list_config_api(cmd_data, cmd_data_len);
        break;
    case CMD_LIST_CONFIG_FIELD:
        handle_list_config_field(cmd_data, cmd_data_len);
        break;
    case CMD_SET_NETWORK:
        handle_set_network(cmd_data, cmd_data_len);
        break;
    case CMD_SET_NETWORK_BEGIN:
        handle_set_network_begin(cmd_data, cmd_data_len);
        break;
    case CMD_SET_NETWORK_API:
        handle_set_network_api(cmd_data, cmd_data_len);
        break;
    case CMD_SET_NETWORK_FIELD:
        handle_set_network_field(cmd_data, cmd_data_len);
        break;
    case CMD_SET_NETWORK_COMMIT:
        handle_set_network_commit(cmd_data, cmd_data_len);
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
    bool first_byte_logged = false;

    send_state(STATE_READY);

    while (s_run) {
        uint8_t b;
        if (uart_read_one(&b, 100) != 1) continue;
        /* One-shot diagnostic: proves host->device USB-Serial-JTAG works.
         * If RPCs hang in the browser but this never fires, the host port
         * handle is stale (typically: chip soft-reset re-enumerated as a new
         * USB device, browser is still writing to the old one). */
        if (!first_byte_logged) {
            ESP_LOGI(TAG, "First byte from host: 0x%02X", b);
            first_byte_logged = true;
        }
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
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
    if (s_usb_serial_jtag_active) {
        usb_serial_jtag_driver_uninstall();
        s_usb_serial_jtag_active = false;
    }
#endif
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t improv_start(EventGroupHandle_t done_events, EventBits_t done_bit)
{
    if (s_task) return ESP_OK;  /* already running */

    s_done_events = done_events;
    s_done_bit = done_bit;

#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB-Serial-JTAG driver: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_usb_serial_jtag_active = true;
    ESP_LOGI(TAG, "Improv ready on USB-Serial-JTAG");
#else
    s_uart_fd = open("/dev/uart/0", O_RDWR | O_NONBLOCK);
    if (s_uart_fd < 0) {
        ESP_LOGE(TAG, "Failed to open /dev/uart/0: %d", errno);
        return ESP_FAIL;
    }
#endif

    s_run = true;
    /* 20 KiB stack: handle_reorder_networks keeps two 7+ KiB cfg_v2 structs
     * in flight at once; remaining headroom covers cJSON frame overhead and
     * FreeRTOS bookkeeping. */
    BaseType_t ret = xTaskCreate(improv_task, "improv", 20480, NULL, 5, &s_task);
    if (ret != pdPASS) {
        s_run = false;
#if CONFIG_DEVDASH_IMPROV_USB_SERIAL_JTAG
        if (s_usb_serial_jtag_active) {
            usb_serial_jtag_driver_uninstall();
            s_usb_serial_jtag_active = false;
        }
#else
        close(s_uart_fd);
        s_uart_fd = -1;
#endif
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
