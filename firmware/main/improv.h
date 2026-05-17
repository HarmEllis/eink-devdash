#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* Start the Improv WiFi Serial provisioning task on UART0 (115200 baud).
 *
 * The task listens for Improv Serial packets from a host (typically the
 * browser-based web flasher) and, when WiFi credentials are received, stores
 * them via esp_wifi_set_config and attempts to connect. On a successful
 * connection it sets `done_bit` on `done_events`, so the SoftAP provisioning
 * loop in wifi_prov.c can exit and continue with normal boot.
 *
 * Safe to call multiple times — subsequent calls are no-ops while a task is
 * already running. Use improv_stop() to terminate the task before deep sleep
 * or before deinitialising WiFi.
 */
esp_err_t improv_start(EventGroupHandle_t done_events, EventBits_t done_bit);

/* Stop the Improv task and close UART0. Safe to call if not started. */
void improv_stop(void);
