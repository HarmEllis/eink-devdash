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

/* If no runtime profile exists, start the device-hosted SoftAP HTTP portal and
 * block until the user has supplied WiFi and API settings (or until the
 * Kconfig timeout fires). Returns ESP_OK if the device is provisioned by the
 * time the call returns. */
esp_err_t wifi_net_provision_if_needed(void);

/* Open the SoftAP provisioning and Improv Serial configuration window even
 * when credentials already exist. Used for GPIO0 wake-up and explicit rescue
 * flows. */
esp_err_t wifi_net_open_config_window(void);

/* Stop WiFi (call before deep sleep). */
void wifi_net_stop(void);

/* Get the SoftAP provisioning SSID and AP password that will be advertised by
 * wifi_net_provision_if_needed(). Both are derived from the factory MAC, so
 * they are stable per device. Use this to render them on the e-ink display. */
void wifi_net_get_prov_info(char *ssid, size_t ssid_sz,
                            char *pop, size_t pop_sz);
