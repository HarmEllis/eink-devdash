#pragma once

#include "esp_err.h"
#include "storage.h"
#include "unreachable_diag.h"

esp_err_t wifi_roam_connect(dash_config_v2_t *cfg,
                            int *network_idx_out,
                            wifi_unreachable_diag_t *diag);
