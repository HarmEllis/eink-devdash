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
void display_show_offline(void);
