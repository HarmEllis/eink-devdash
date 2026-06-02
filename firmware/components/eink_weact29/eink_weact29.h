#pragma once
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include <stdbool.h>

/* WeAct 2.9" Black/Red — SSD1680, 128×296 px */
#define EINK_WIDTH    128
#define EINK_HEIGHT   296
#define EINK_BUF_SIZE (EINK_WIDTH * EINK_HEIGHT / 8)  /* 4736 bytes */

/* Pin mapping — ESP32-S3 Super Mini */
#define EINK_PIN_MOSI  CONFIG_EINK_SPI_MOSI   /* SDA (yellow) */
#define EINK_PIN_SCK   CONFIG_EINK_SPI_SCK    /* SCL (green)  */
#define EINK_PIN_CS    CONFIG_EINK_SPI_CS     /* CS  (blue)   */
#define EINK_PIN_DC    CONFIG_EINK_DC_PIN     /* D/C (white)  */
#define EINK_PIN_RST   CONFIG_EINK_RST_PIN    /* RES (orange) */
#define EINK_PIN_BUSY  CONFIG_EINK_BUSY_PIN   /* BUSY (purple) */
/* MISO not connected (-1), VCC = 3V3 */

typedef enum {
    EINK_PANEL_WEACT_29_BWR = 0,   /* WeAct 2.9" Black/White/Red */
    EINK_PANEL_WEACT_29_BW  = 1,   /* WeAct 2.9" Black/White */
} eink_panel_variant_t;

_Static_assert(EINK_PANEL_WEACT_29_BW <= 0xFF,
               "eink_panel_variant_t must fit in uint8_t");

typedef enum {
    EINK_REFRESH_BW_FAST,    /* BWR experiment, Mode 2 LUT (inert in product) */
    EINK_REFRESH_FULL_COLOR, /* BWR full color, Mode 1 LUT */
    EINK_REFRESH_BW_FULL,    /* BW panel, stock OTP GC waveform.
                                Requires h->variant == EINK_PANEL_WEACT_29_BW. */
    EINK_REFRESH_SAFE_BW,    /* BW-only refresh used by provisioning /
                                recovery surfaces. No custom LUT load.
                                BWR writes a no-red 0x26 plane; known-BW
                                writes mono old/base RAM (0x26) as well. */
} eink_refresh_mode_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} eink_rect_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t dc_pin;
    gpio_num_t rst_pin;
    gpio_num_t busy_pin;
    bool asleep;
    eink_panel_variant_t variant;
} eink_handle_t;

/* Initialize SPI bus + device and bring the controller to a known reset
   state. Mode-agnostic: programs no OTP/LUT/0x21 bytes. The first
   eink_refresh() call after init runs the variant- and mode-appropriate
   reset_controller_* helper because h->asleep is left true here. */
esp_err_t eink_init(eink_handle_t *h, eink_panel_variant_t variant);

/* Copy buffers into internal framebuffers (NULL = keep current). */
void eink_set_framebuffer(const uint8_t *bw_buf, const uint8_t *red_buf);

/* Trigger a display update.

   Dispatch rules:
   - EINK_REFRESH_SAFE_BW    — always reset_controller_safe_bw(h); writes
                                0x24 plus a panel-appropriate 0x26 plane.
   - EINK_REFRESH_BW_FAST    — BWR experiment path (current behavior).
   - EINK_REFRESH_BW_FULL    — requires h->variant == EINK_PANEL_WEACT_29_BW;
                                reset_controller_bw_full(h), 0x24 + 0x26.
   - EINK_REFRESH_FULL_COLOR — requires h->variant == EINK_PANEL_WEACT_29_BWR;
                                BWR full reset on wake, writes 0x24 + 0x26. */
void eink_refresh(eink_handle_t *h, eink_refresh_mode_t mode);

/* Trigger a black/white partial update.

   - BWR variant: prev_bw is ignored; behaves as the current Mode-2 LUT path.
   - BW variant:  prev_bw is written into the old-frame RAM (0x26), next_bw
                  into the new-frame RAM (0x24); the DU trigger then diffs
                  the two RAMs over the window. Callers must pass the buffer
                  that matches what is currently displayed for prev_bw, and
                  must only commit their "last displayed" snapshot after this
                  call returns true. */
bool eink_refresh_bw_partial(eink_handle_t *h,
                             const uint8_t *prev_bw,
                             const uint8_t *next_bw,
                             eink_rect_t rect);

/* Send panel to deep sleep (call after every refresh). */
void eink_sleep(eink_handle_t *h);
