#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "storage.h"

/* Initialize TCP/IP stack, default event loop, and WiFi driver (STA mode).
 * Must be called once before any other wifi_* function. */
esp_err_t wifi_net_init(void);

/* True if WiFi credentials have already been stored (provisioning previously
 * completed successfully). Safe to call after wifi_net_init(). */
bool wifi_net_is_provisioned(void);

/* If WiFi credentials are not yet stored, start the SoftAP-based provisioning
 * manager and block until the user has supplied credentials (or until the
 * Kconfig timeout fires). Returns ESP_OK if the device is provisioned by the
 * time the call returns. */
esp_err_t wifi_net_provision_if_needed(void);

/* Connect using stored credentials and wait for an IP address, up to the
 * Kconfig WiFi connect timeout. */
esp_err_t wifi_net_connect(void);

/* Stop WiFi (call before deep sleep). */
void wifi_net_stop(void);

/* Clear stored STA credentials so the next provisioning round runs again.
 * Used as a fallback when stored credentials no longer connect (e.g. user
 * changed their WiFi password). Returns the result of esp_wifi_set_config. */
esp_err_t wifi_net_forget(void);

/* Get the SoftAP provisioning SSID and Proof-of-Possession that will be
 * advertised by wifi_net_provision_if_needed(). Both are derived from the
 * factory MAC, so they are stable per device. Use this to render them on
 * the e-ink display alongside the QR code. */
void wifi_net_get_prov_info(char *ssid, size_t ssid_sz,
                            char *pop, size_t pop_sz);
