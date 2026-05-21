#pragma once
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>

/* WeAct 2.9" Black/Red — SSD1680, 128×296 px */
#define EINK_WIDTH    128
#define EINK_HEIGHT   296
#define EINK_BUF_SIZE (EINK_WIDTH * EINK_HEIGHT / 8)  /* 4736 bytes */

/* Pin mapping — ESP32-S3 Super Mini */
#define EINK_PIN_MOSI  11   /* SDA (yellow) */
#define EINK_PIN_SCK   12   /* SCL (green)  */
#define EINK_PIN_CS    10   /* CS  (blue)   */
#define EINK_PIN_DC     9   /* D/C (white)  */
#define EINK_PIN_RST    1   /* RES (orange) */
#define EINK_PIN_BUSY  13   /* BUSY (purple) */
/* MISO not connected (-1), VCC = 3V3 */

typedef enum {
    EINK_REFRESH_BW_FAST,    /* ~2-4s,   BW RAM only, Mode 2 LUT */
    EINK_REFRESH_FULL_COLOR, /* ~15-27s, BW+Red RAM,  Mode 1 LUT */
} eink_refresh_mode_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t dc_pin;
    gpio_num_t rst_pin;
    gpio_num_t busy_pin;
    bool asleep;
} eink_handle_t;

/* Initializes SPI bus + device and resets the panel. */
esp_err_t eink_init(eink_handle_t *h);

/* Copy buffers into internal framebuffers (NULL = keep current). */
void eink_set_framebuffer(const uint8_t *bw_buf, const uint8_t *red_buf);

/* Copy the previous black/white framebuffer used by differential partial
 * refreshes. The SSD1680 uses command 0x26 as the previous BW plane in
 * monochrome partial mode. */
void eink_set_previous_bw_framebuffer(const uint8_t *bw_buf);

/* Trigger a display update. */
void eink_refresh(eink_handle_t *h, eink_refresh_mode_t mode);

/* Trigger a BW-only update for a physical panel area. The x range is expanded
 * to byte boundaries because SSD1680 RAM addresses pixels in 8-pixel columns. */
void eink_refresh_bw_area(eink_handle_t *h, int x, int y, int width, int height);

/* Send panel to deep sleep (call after every refresh). */
void eink_sleep(eink_handle_t *h);
