#include "eink_weact29.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eink";

/* SSD1680 commands */
#define CMD_DRIVER_OUTPUT    0x01
#define CMD_WRITE_BW_RAM     0x24
#define CMD_WRITE_RED_RAM    0x26
#define CMD_SET_RAM_X_ADDR   0x44
#define CMD_SET_RAM_Y_ADDR   0x45
#define CMD_SET_RAM_X_COUNT  0x4E
#define CMD_SET_RAM_Y_COUNT  0x4F
#define CMD_DISP_UPDATE_CTRL 0x22
#define CMD_MASTER_ACTIVATE  0x20
#define CMD_DEEP_SLEEP       0x10

static uint8_t bw_framebuf[EINK_BUF_SIZE];
static uint8_t red_framebuf[EINK_BUF_SIZE];

static void wait_busy(const eink_handle_t *h)
{
    while (gpio_get_level(h->busy_pin) == 1)
        vTaskDelay(pdMS_TO_TICKS(5));
}

static void send_cmd(const eink_handle_t *h, uint8_t cmd)
{
    gpio_set_level(h->dc_pin, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_transmit(h->spi, &t);
}

static void send_data(const eink_handle_t *h, const uint8_t *data, size_t len)
{
    gpio_set_level(h->dc_pin, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_transmit(h->spi, &t);
}

static void send_byte(const eink_handle_t *h, uint8_t byte)
{
    send_data(h, &byte, 1);
}

esp_err_t eink_init(eink_handle_t *h)
{
    /* HW reset */
    gpio_set_level(h->rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_busy(h);

    /* Driver output — 296 gates, scan down, mirror */
    send_cmd(h, CMD_DRIVER_OUTPUT);
    uint8_t drv[] = { 0x27, 0x01, 0x00 }; /* 0x0127 = 295 */
    send_data(h, drv, 3);

    ESP_LOGI(TAG, "SSD1680 initialized");
    return ESP_OK;
}

void eink_set_framebuffer(const uint8_t *bw_buf, const uint8_t *red_buf)
{
    if (bw_buf) memcpy(bw_framebuf, bw_buf, EINK_BUF_SIZE);
    if (red_buf) memcpy(red_framebuf, red_buf, EINK_BUF_SIZE);
}

void eink_refresh(eink_handle_t *h, eink_refresh_mode_t mode)
{
    /* Set RAM address window */
    send_cmd(h, CMD_SET_RAM_X_ADDR);
    send_byte(h, 0x00); send_byte(h, (EINK_WIDTH / 8) - 1);
    send_cmd(h, CMD_SET_RAM_Y_ADDR);
    send_byte(h, 0x00); send_byte(h, 0x00);
    send_byte(h, (EINK_HEIGHT - 1) & 0xFF);
    send_byte(h, ((EINK_HEIGHT - 1) >> 8) & 0xFF);

    /* Write BW RAM */
    send_cmd(h, CMD_SET_RAM_X_COUNT); send_byte(h, 0x00);
    send_cmd(h, CMD_SET_RAM_Y_COUNT); send_byte(h, 0x00); send_byte(h, 0x00);
    send_cmd(h, CMD_WRITE_BW_RAM);
    send_data(h, bw_framebuf, EINK_BUF_SIZE);

    if (mode == EINK_REFRESH_FULL_COLOR) {
        /* Write Red RAM */
        send_cmd(h, CMD_SET_RAM_X_COUNT); send_byte(h, 0x00);
        send_cmd(h, CMD_SET_RAM_Y_COUNT); send_byte(h, 0x00); send_byte(h, 0x00);
        send_cmd(h, CMD_WRITE_RED_RAM);
        send_data(h, red_framebuf, EINK_BUF_SIZE);
    }

    /* Trigger update */
    send_cmd(h, CMD_DISP_UPDATE_CTRL);
    if (mode == EINK_REFRESH_BW_FAST) {
        send_byte(h, 0xFF); /* Mode 2 + temp load */
    } else {
        send_byte(h, 0xF7); /* Mode 1 full */
    }
    send_cmd(h, CMD_MASTER_ACTIVATE);
    wait_busy(h);

    ESP_LOGI(TAG, "Refresh done (mode=%d)", mode);
}

void eink_sleep(eink_handle_t *h)
{
    send_cmd(h, CMD_DEEP_SLEEP);
    send_byte(h, 0x01);
}
