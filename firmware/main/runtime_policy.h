#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DASH_REFRESH_MIN_STANDARD                3
#define DASH_REFRESH_MIN_BW_TWO_PARTIALS         1
#define DASH_REFRESH_MAX                         60
#define DASH_REFRESH_BW_SHORT_INTERVAL_MIN_PARTIALS 2

bool clock_should_apply(const char *iso, bool stale);
bool api_url_is_relay(const char *url);

/* True iff `cc` is a WiFi regulatory country code esp_wifi_set_country_code()
 * accepts on ESP-IDF 5.3 (the "01" world-safe sentinel or a supported two-letter
 * ISO code). Single source of truth for stored values, the build-time default,
 * and portal save validation. Pure (no NVS/IDF deps) so it is host-testable. */
bool wifi_country_is_supported(const char *cc);
uint8_t dashboard_refresh_input_minimum(bool is_bw);
uint8_t dashboard_refresh_minimum(bool is_bw, uint8_t max_partials);
bool dashboard_refresh_config_is_valid(uint8_t refresh_min, bool is_bw,
                                       uint8_t max_partials);
bool offline_partial_refresh_allowed(uint8_t partial_count,
                                     uint8_t max_partials,
                                     uint16_t renders_since_full,
                                     uint16_t render_cap);
