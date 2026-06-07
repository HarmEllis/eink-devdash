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

void boot_button_monitor_start(void);

#ifdef __cplusplus
}
#endif
