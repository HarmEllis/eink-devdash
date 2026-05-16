#pragma once
#include "driver/spi_master.h"
#include "driver/gpio.h"

/* WeAct 2.9" Black/Red — SSD1680, 128×296 px */
#define EINK_WIDTH  128
#define EINK_HEIGHT 296
#define EINK_BUF_SIZE (EINK_WIDTH * EINK_HEIGHT / 8)  /* 4736 bytes */

typedef enum {
    EINK_REFRESH_BW_FAST,   /* ~2-4s, BW RAM only, Mode 2 LUT */
    EINK_REFRESH_FULL_COLOR, /* ~15-27s, BW+Red RAM, Mode 1 LUT */
} eink_refresh_mode_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t dc_pin;
    gpio_num_t rst_pin;
    gpio_num_t busy_pin;
} eink_handle_t;

esp_err_t eink_init(eink_handle_t *h);
void eink_set_framebuffer(const uint8_t *bw_buf, const uint8_t *red_buf);
void eink_refresh(eink_handle_t *h, eink_refresh_mode_t mode);
void eink_sleep(eink_handle_t *h);
