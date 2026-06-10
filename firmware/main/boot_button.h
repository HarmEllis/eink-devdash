#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void boot_button_init(void);
bool boot_button_is_pressed(void);

bool boot_button_wait_longpress(uint32_t hold_ms);
void boot_button_wait_release(void);

void boot_button_force_prov_mark(void);
bool boot_button_force_prov_active(void);
void boot_button_force_prov_clear(void);

/* Device-side factory reset, armed from the setup-screen BOOT gesture. The flag
 * lives in RTC_NOINIT memory (must survive the gesture's esp_restart(), which
 * RTC_DATA_ATTR would not) so app_main can erase the whole NVS partition on the
 * next boot, before nvs_flash_init(). Lets the user fully reset without the web
 * flasher. NOTE: RTC_NOINIT is undefined after a cold/power-on boot, so the
 * DESTRUCTIVE consumer MUST also require a software-reset reason (ESP_RST_SW)
 * before acting on a pending flag — see app_main(). */
void boot_button_request_factory_reset(void);
bool boot_button_factory_reset_pending(void);
void boot_button_factory_reset_clear(void);

/* Edge-counted BOOT presses for the setup reset gesture. The confirm-screen
 * countdown blocks for ~1.4 s per BW partial refresh, during which the gesture
 * loop cannot poll the button; a GPIO falling-edge ISR latches each press into a
 * debounced counter so taps (incl. a fast double-tap for full erase) are not
 * lost. arm() zeroes the count and enables the ISR; take() atomically
 * reads-and-clears the accumulated count; disarm() disables the ISR so the
 * normal level-polling paths (long-press, wait_release) are unaffected.
 * If arm() cannot install the ISR it logs and returns: take() then stays 0, so
 * the gesture safely degrades to "cancel" (no destructive action). */
void boot_button_press_counter_arm(void);
int  boot_button_press_counter_take(void);
void boot_button_press_counter_disarm(void);

void boot_button_monitor_start(void);

#ifdef __cplusplus
}
#endif
