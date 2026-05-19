#pragma once
#include "api_client.h"
#include "storage.h"

void display_render(const dashboard_data_t *data);
/* Render the provisioning prompt with the actual per-device SoftAP SSID and
 * AP password. Pass NULL for either to fall back to a placeholder. */
void display_show_qr(const char *ssid, const char *pop);
void display_show_offline(void);
