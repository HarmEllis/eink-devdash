#include "improv.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "improv";

#define IMPROV_BUF_SIZE   256

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
    uint8_t pkt[IMPROV_BUF_SIZE];
    int pos = 0;
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
