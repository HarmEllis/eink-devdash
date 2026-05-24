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
#define CMD_WRITE_TEMP_REG      0x1A
#define CMD_DISP_UPDATE_CTRL1   0x21
#define CMD_DISP_UPDATE_CTRL2   0x22
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
static uint8_t partial_framebuf[EINK_BUF_SIZE];

#define EINK_ROW_BYTES (EINK_WIDTH / 8)

static void wait_busy(const eink_handle_t *h)
{
    /* BUSY is HIGH while panel is busy. Use vTaskDelay(1) — at least one full
       tick — so IDLE0 gets CPU time and the task watchdog doesn't trigger
       during the ~15s BWR full refresh. */
    while (gpio_get_level(h->busy_pin) == 1)
        vTaskDelay(1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static bool wait_busy_timeout(const eink_handle_t *h, TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(h->busy_pin) == 1) {
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            return false;
        }
        vTaskDelay(1);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
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

static void set_ram_window(const eink_handle_t *h,
                           uint16_t x_byte_start,
                           uint16_t x_byte_end,
                           uint16_t y_start,
                           uint16_t y_end)
{
    send_cmd(h, CMD_SET_RAM_X_ADDR);
    send_byte(h, x_byte_start & 0xFF);
    send_byte(h, x_byte_end & 0xFF);

    send_cmd(h, CMD_SET_RAM_Y_ADDR);
    send_byte(h, y_start & 0xFF);
    send_byte(h, (y_start >> 8) & 0xFF);
    send_byte(h, y_end & 0xFF);
    send_byte(h, (y_end >> 8) & 0xFF);
}

static void set_ram_counter(const eink_handle_t *h,
                            uint16_t x_byte,
                            uint16_t y)
{
    send_cmd(h, CMD_SET_RAM_X_COUNT);
    send_byte(h, x_byte & 0xFF);
    send_cmd(h, CMD_SET_RAM_Y_COUNT);
    send_byte(h, y & 0xFF);
    send_byte(h, (y >> 8) & 0xFF);
}

static void write_full_plane(const eink_handle_t *h, uint8_t cmd,
                             const uint8_t *buf)
{
    set_ram_window(h, 0, EINK_ROW_BYTES - 1, 0, EINK_HEIGHT - 1);
    set_ram_counter(h, 0, 0);
    send_cmd(h, cmd);
    send_data(h, buf, EINK_BUF_SIZE);
}

static void write_bw_window(const eink_handle_t *h, const uint8_t *buf,
                            eink_rect_t rect)
{
    uint16_t x_byte = rect.x / 8;
    uint16_t x_bytes = rect.w / 8;
    uint16_t y_end = rect.y + rect.h - 1;
    size_t len = (size_t)x_bytes * rect.h;

    set_ram_window(h, x_byte, x_byte + x_bytes - 1, rect.y, y_end);
    set_ram_counter(h, x_byte, rect.y);
    send_cmd(h, CMD_WRITE_BW_RAM);

    if (x_bytes == EINK_ROW_BYTES) {
        send_data(h, buf + rect.y * EINK_ROW_BYTES, len);
        return;
    }

    for (uint16_t row = 0; row < rect.h; row++) {
        memcpy(partial_framebuf + row * x_bytes,
               buf + (rect.y + row) * EINK_ROW_BYTES + x_byte,
               x_bytes);
    }
    send_data(h, partial_framebuf, len);
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
    if (!wait_busy_timeout(h, pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "BUSY timeout after hardware reset");
    }

    /* Software reset — required after hardware reset */
    send_cmd(h, CMD_SW_RESET);
    if (!wait_busy_timeout(h, pdMS_TO_TICKS(5000))) {
        ESP_LOGW(TAG, "BUSY timeout after software reset");
    }

    /* Driver output control — 296 gates, GD=0, SM=0, TB=0 */
    send_cmd(h, CMD_DRIVER_OUTPUT);
    uint8_t drv[] = { (EINK_HEIGHT - 1) & 0xFF, ((EINK_HEIGHT - 1) >> 8) & 0x01, 0x00 };
    send_data(h, drv, sizeof(drv));

    /* Data entry mode: X increment, Y increment, address counter updates in X direction */
    send_cmd(h, CMD_DATA_ENTRY_MODE);
    send_byte(h, 0x03);

    /* Set RAM address windows (must match what refresh operations send) */
    set_ram_window(h, 0, EINK_ROW_BYTES - 1, 0, EINK_HEIGHT - 1);

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

static void reset_controller_fast_bw(eink_handle_t *h)
{
    /* This follows Waveshare's 2.9B V4 Init_Fast sequence. It deliberately
       avoids the color-plane setup and prepares the controller for fast
       black/white-only updates on the tri-color glass. */
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(h->rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(h->rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    wait_busy(h);

    send_cmd(h, CMD_SW_RESET);
    wait_busy(h);

    send_cmd(h, CMD_TEMP_SENSOR_CTRL);
    send_byte(h, 0x80);
    send_cmd(h, CMD_DISP_UPDATE_CTRL2);
    send_byte(h, 0xB1);
    send_cmd(h, CMD_MASTER_ACTIVATE);
    wait_busy(h);

    send_cmd(h, CMD_WRITE_TEMP_REG);
    send_byte(h, 0x5A);
    send_byte(h, 0x00);
    send_cmd(h, CMD_DISP_UPDATE_CTRL2);
    send_byte(h, 0x91);
    send_cmd(h, CMD_MASTER_ACTIVATE);
    wait_busy(h);

    send_cmd(h, CMD_DRIVER_OUTPUT);
    uint8_t drv[] = { (EINK_HEIGHT - 1) & 0xFF, ((EINK_HEIGHT - 1) >> 8) & 0x01, 0x00 };
    send_data(h, drv, sizeof(drv));

    send_cmd(h, CMD_DATA_ENTRY_MODE);
    send_byte(h, 0x03);

    set_ram_window(h, 0, EINK_ROW_BYTES - 1, 0, EINK_HEIGHT - 1);
    set_ram_counter(h, 0, 0);
    wait_busy(h);

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

void eink_refresh(eink_handle_t *h, eink_refresh_mode_t mode)
{
    if (mode == EINK_REFRESH_BW_FAST) {
        reset_controller_fast_bw(h);
        ESP_LOGI(TAG, "SSD1680 initialized for BW fast");
    } else if (h->asleep) {
        reset_controller(h);
        ESP_LOGI(TAG, "SSD1680 woke from deep sleep");
    }

    write_full_plane(h, CMD_WRITE_BW_RAM, bw_framebuf);

    if (mode == EINK_REFRESH_FULL_COLOR) {
        write_full_plane(h, CMD_WRITE_RED_RAM, red_framebuf);
    } else {
        /* On this panel + init sequence, 0xFF in red RAM paints all-red.
           Preserve the normal project convention: 0x00 means no red. */
        write_full_plane(h, CMD_WRITE_RED_RAM, red_framebuf);
    }

    send_cmd(h, CMD_DISP_UPDATE_CTRL2);
    send_byte(h, mode == EINK_REFRESH_BW_FAST ? 0xC7 : 0xF7);
    send_cmd(h, CMD_MASTER_ACTIVATE);
    wait_busy(h);

    ESP_LOGI(TAG, "Refresh done (mode=%s)",
             mode == EINK_REFRESH_BW_FAST ? "BW_FAST" : "FULL_COLOR");
}

bool eink_refresh_bw_partial(eink_handle_t *h,
                             const uint8_t *next_bw,
                             eink_rect_t rect)
{
    if (!next_bw || rect.w == 0 || rect.h == 0) {
        return false;
    }

    uint16_t x_end = rect.x + rect.w;
    uint16_t y_end = rect.y + rect.h;
    if ((rect.x % 8) != 0 || (rect.w % 8) != 0 ||
        x_end > EINK_WIDTH || y_end > EINK_HEIGHT) {
        ESP_LOGW(TAG, "Invalid partial rect x=%u y=%u w=%u h=%u",
                 rect.x, rect.y, rect.w, rect.h);
        return false;
    }

    reset_controller_fast_bw(h);
    ESP_LOGI(TAG, "SSD1680 initialized for BW partial");

    write_bw_window(h, next_bw, rect);

    ESP_LOGI(TAG, "Activating BW partial update");
    send_cmd(h, CMD_DISP_UPDATE_CTRL2);
    send_byte(h, 0x1C);
    send_cmd(h, CMD_MASTER_ACTIVATE);
    if (!wait_busy_timeout(h, pdMS_TO_TICKS(30000))) {
        ESP_LOGW(TAG, "BW partial timed out waiting for BUSY release");
        reset_controller(h);
        ESP_LOGW(TAG, "SSD1680 reset after failed BW partial");
        return false;
    }

    ESP_LOGI(TAG, "Refresh done (mode=BW_PARTIAL x=%u y=%u w=%u h=%u)",
             rect.x, rect.y, rect.w, rect.h);
    return true;
}

void eink_sleep(eink_handle_t *h)
{
    send_cmd(h, CMD_DEEP_SLEEP);
    send_byte(h, 0x01);
    h->asleep = true;
}
