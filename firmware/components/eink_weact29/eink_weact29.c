#include "eink_weact29.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "eink";

/* SSD1680 commands */
#define CMD_DRIVER_OUTPUT       0x01
#define CMD_SW_RESET            0x12
#define CMD_TEMP_SENSOR_CTRL    0x18
#define CMD_DISP_UPDATE_CTRL1   0x21
#define CMD_DISP_UPDATE_CTRL    0x22
#define CMD_WRITE_BW_RAM        0x24
#define CMD_WRITE_RED_RAM       0x26
#define CMD_DATA_ENTRY_MODE     0x11
#define CMD_BORDER_WAVEFORM     0x3C
#define CMD_SET_RAM_X_ADDR      0x44
#define CMD_SET_RAM_Y_ADDR      0x45
#define CMD_SET_RAM_X_COUNT     0x4E
#define CMD_SET_RAM_Y_COUNT     0x4F
#define CMD_MASTER_ACTIVATE     0x20
#define CMD_DEEP_SLEEP          0x10

static uint8_t bw_framebuf[EINK_BUF_SIZE];
static uint8_t red_framebuf[EINK_BUF_SIZE];
static uint8_t previous_bw_framebuf[EINK_BUF_SIZE];

static void wait_busy(const eink_handle_t *h)
{
    /* BUSY is HIGH while panel is busy. Use vTaskDelay(1) — at least one full
       tick — so IDLE0 gets CPU time and the task watchdog doesn't trigger
       during the ~15s BWR full refresh. */
    while (gpio_get_level(h->busy_pin) == 1)
        vTaskDelay(1);
}

static void send_cmd(const eink_handle_t *h, uint8_t cmd)
{
    gpio_set_level(h->dc_pin, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(h->spi, &t);
}

static void send_data(const eink_handle_t *h, const uint8_t *data, size_t len)
{
    gpio_set_level(h->dc_pin, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(h->spi, &t);
}

static void send_byte(const eink_handle_t *h, uint8_t byte)
{
    send_data(h, &byte, 1);
}

static void reset_controller(eink_handle_t *h)
{
    /* Hardware reset */
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(h->rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_busy(h);

    /* Software reset — required after hardware reset */
    send_cmd(h, CMD_SW_RESET);
    wait_busy(h);

    /* Driver output control — 296 gates, GD=0, SM=0, TB=0 */
    send_cmd(h, CMD_DRIVER_OUTPUT);
    uint8_t drv[] = { (EINK_HEIGHT - 1) & 0xFF, ((EINK_HEIGHT - 1) >> 8) & 0x01, 0x00 };
    send_data(h, drv, sizeof(drv));

    /* Data entry mode: X increment, Y increment, address counter updates in X direction */
    send_cmd(h, CMD_DATA_ENTRY_MODE);
    send_byte(h, 0x03);

    /* Set RAM address windows (must match what eink_refresh sends) */
    send_cmd(h, CMD_SET_RAM_X_ADDR);
    send_byte(h, 0x00);
    send_byte(h, (EINK_WIDTH / 8) - 1);

    send_cmd(h, CMD_SET_RAM_Y_ADDR);
    send_byte(h, 0x00); send_byte(h, 0x00);
    send_byte(h, (EINK_HEIGHT - 1) & 0xFF);
    send_byte(h, ((EINK_HEIGHT - 1) >> 8) & 0xFF);

    /* Border waveform: VSS (avoids red border artefacts) */
    send_cmd(h, CMD_BORDER_WAVEFORM);
    send_byte(h, 0x05);

    /* Display Update Control 1: normal BW source (0x00), normal red source (0x80).
       Without 0x80 in byte 2 the red channel polarity is inverted after SW reset —
       0xFF in red RAM renders as all-red instead of all-white. */
    send_cmd(h, CMD_DISP_UPDATE_CTRL1);
    send_byte(h, 0x00);
    send_byte(h, 0x80);

    /* Use internal temperature sensor for waveform selection */
    send_cmd(h, CMD_TEMP_SENSOR_CTRL);
    send_byte(h, 0x80);

    h->asleep = false;
}

esp_err_t eink_init(eink_handle_t *h)
{
    h->dc_pin   = EINK_PIN_DC;
    h->rst_pin  = EINK_PIN_RST;
    h->busy_pin = EINK_PIN_BUSY;

    /* GPIO config for DC, RST, BUSY */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EINK_PIN_DC) | (1ULL << EINK_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << EINK_PIN_BUSY),
        .mode         = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* SPI bus — MISO unused. max_transfer_sz must be >= EINK_BUF_SIZE (4736);
       default is 4096 which silently drops the framebuffer write. */
    spi_bus_config_t bus = {
        .mosi_io_num    = EINK_PIN_MOSI,
        .miso_io_num    = -1,
        .sclk_io_num    = EINK_PIN_SCK,
        .quadwp_io_num  = -1,
        .quadhd_io_num  = -1,
        .max_transfer_sz = EINK_BUF_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    /* SPI device — CS managed manually via DC line convention,
       but we still register CS pin so the driver controls it */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,  /* 4 MHz — SSD1680 max 20 MHz, conservative start */
        .mode           = 0,
        .spics_io_num   = EINK_PIN_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &h->spi));

    reset_controller(h);

    ESP_LOGI(TAG, "SSD1680 initialized (MOSI=%d SCK=%d CS=%d DC=%d RST=%d BUSY=%d)",
             EINK_PIN_MOSI, EINK_PIN_SCK, EINK_PIN_CS,
             EINK_PIN_DC, EINK_PIN_RST, EINK_PIN_BUSY);

    return ESP_OK;
}

void eink_set_framebuffer(const uint8_t *bw_buf, const uint8_t *red_buf)
{
    if (bw_buf)  memcpy(bw_framebuf,  bw_buf,  EINK_BUF_SIZE);
    if (red_buf) memcpy(red_framebuf, red_buf, EINK_BUF_SIZE);
}

void eink_set_previous_bw_framebuffer(const uint8_t *bw_buf)
{
    if (bw_buf) memcpy(previous_bw_framebuf, bw_buf, EINK_BUF_SIZE);
}

void eink_refresh(eink_handle_t *h, eink_refresh_mode_t mode)
{
    if (h->asleep) {
        reset_controller(h);
        if (mode == EINK_REFRESH_BW_FAST) {
            mode = EINK_REFRESH_FULL_COLOR;
        }
        ESP_LOGI(TAG, "SSD1680 woke from deep sleep");
    }

    /* Set RAM X address window: 0 .. (WIDTH/8 - 1) */
    send_cmd(h, CMD_SET_RAM_X_ADDR);
    send_byte(h, 0x00);
    send_byte(h, (EINK_WIDTH / 8) - 1);

    /* Set RAM Y address window: 0 .. (HEIGHT - 1) */
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
        send_cmd(h, CMD_SET_RAM_X_COUNT); send_byte(h, 0x00);
        send_cmd(h, CMD_SET_RAM_Y_COUNT); send_byte(h, 0x00); send_byte(h, 0x00);
        send_cmd(h, CMD_WRITE_RED_RAM);
        send_data(h, red_framebuf, EINK_BUF_SIZE);
    }

    send_cmd(h, CMD_DISP_UPDATE_CTRL);
    send_byte(h, mode == EINK_REFRESH_BW_FAST ? 0xFF : 0xF7);
    send_cmd(h, CMD_MASTER_ACTIVATE);
    wait_busy(h);

    ESP_LOGI(TAG, "Refresh done (mode=%s)",
             mode == EINK_REFRESH_BW_FAST ? "BW_FAST" : "FULL_COLOR");
}

void eink_refresh_bw_area(eink_handle_t *h, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width - 1;
    int y1 = y + height - 1;
    if (x1 >= EINK_WIDTH) x1 = EINK_WIDTH - 1;
    if (y1 >= EINK_HEIGHT) y1 = EINK_HEIGHT - 1;
    if (x0 > x1 || y0 > y1) return;

    int xb0 = x0 / 8;
    int xb1 = x1 / 8;
    int row_bytes = xb1 - xb0 + 1;
    int stride = EINK_WIDTH / 8;

    if (h->asleep) {
        reset_controller(h);
        ESP_LOGI(TAG, "SSD1680 woke from deep sleep");
    }

    send_cmd(h, CMD_SET_RAM_X_ADDR);
    send_byte(h, xb0);
    send_byte(h, xb1);

    send_cmd(h, CMD_SET_RAM_Y_ADDR);
    send_byte(h, y0 & 0xFF);
    send_byte(h, (y0 >> 8) & 0xFF);
    send_byte(h, y1 & 0xFF);
    send_byte(h, (y1 >> 8) & 0xFF);

    send_cmd(h, CMD_SET_RAM_X_COUNT);
    send_byte(h, xb0);
    send_cmd(h, CMD_SET_RAM_Y_COUNT);
    send_byte(h, y0 & 0xFF);
    send_byte(h, (y0 >> 8) & 0xFF);

    /* In monochrome partial mode command 0x26 is the "previous" BW plane.
       On full color refreshes the same command is the red plane. */
    send_cmd(h, CMD_WRITE_RED_RAM);
    for (int row = y0; row <= y1; row++) {
        send_data(h, previous_bw_framebuf + row * stride + xb0, row_bytes);
    }

    send_cmd(h, CMD_SET_RAM_X_COUNT);
    send_byte(h, xb0);
    send_cmd(h, CMD_SET_RAM_Y_COUNT);
    send_byte(h, y0 & 0xFF);
    send_byte(h, (y0 >> 8) & 0xFF);

    send_cmd(h, CMD_WRITE_BW_RAM);
    for (int row = y0; row <= y1; row++) {
        send_data(h, bw_framebuf + row * stride + xb0, row_bytes);
    }

    send_cmd(h, CMD_DISP_UPDATE_CTRL);
    send_byte(h, 0xFC);
    send_cmd(h, CMD_MASTER_ACTIVATE);
    wait_busy(h);

    ESP_LOGI(TAG, "Partial BW refresh done (x=%d..%d y=%d..%d)",
             xb0 * 8, xb1 * 8 + 7, y0, y1);
}

void eink_sleep(eink_handle_t *h)
{
    send_cmd(h, CMD_DEEP_SLEEP);
    send_byte(h, 0x01);
    h->asleep = true;
}
