#pragma once
#include "api_client.h"
#include "storage.h"

void display_render(const dashboard_data_t *data);
/* Render the V4 S1 provisioning prompt with the per-device SoftAP SSID and
 * AP password. The QR is generated from wifi_net_get_wifi_qr at render time.
 * Pass NULL for either to fall back to a placeholder. */
void display_show_qr(const char *ssid, const char *pop);
/* V4 S1 error variant — drawn when the SoftAP fails to start. */
void display_show_setup_failed(void);

typedef enum {
    DISPLAY_OFFLINE_REASON_API,
    DISPLAY_OFFLINE_REASON_WIFI,
    DISPLAY_OFFLINE_REASON_SETUP_TIMEOUT,
} display_offline_reason_t;

void display_show_offline(display_offline_reason_t reason);
