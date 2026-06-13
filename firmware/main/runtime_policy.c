#include "runtime_policy.h"

#include <string.h>

bool clock_should_apply(const char *iso, bool stale)
{
    return iso != NULL && iso[0] != '\0' && !stale;
}

bool wifi_country_is_supported(const char *cc)
{
    if (!cc || strlen(cc) != 2) return false;
    /* The complete set esp_wifi_set_country_code() accepts on ESP-IDF 5.3
     * (esp_wifi.h attention #6), incl. "01" world-safe. Keep in sync if the IDF
     * version bumps. */
    static const char *const SUPPORTED[] = {
        "01", "AT", "AU", "BE", "BG", "BR", "CA", "CH", "CN", "CY", "CZ",
        "DE", "DK", "EE", "ES", "FI", "FR", "GB", "GR", "HK", "HR", "HU",
        "IE", "IN", "IS", "IT", "JP", "KR", "LI", "LT", "LU", "LV", "MT",
        "MX", "NL", "NO", "NZ", "PL", "PT", "RO", "SE", "SI", "SK", "TW", "US",
    };
    for (size_t i = 0; i < sizeof(SUPPORTED) / sizeof(SUPPORTED[0]); i++) {
        if (cc[0] == SUPPORTED[i][0] && cc[1] == SUPPORTED[i][1]) return true;
    }
    return false;
}

bool api_url_is_relay(const char *url)
{
    if (!url || !url[0]) return false;
    const char *scheme = strstr(url, "://");
    const char *path = scheme ? strchr(scheme + 3, '/') : strchr(url, '/');
    if (!path || strncmp(path, "/d/", 3) != 0) return false;
    const char *device = path + 3;
    return device[0] != '\0' && device[0] != '/' &&
           device[0] != '?' && device[0] != '#';
}

uint8_t dashboard_refresh_input_minimum(bool is_bw)
{
    return is_bw ? DASH_REFRESH_MIN_BW_TWO_PARTIALS
                 : DASH_REFRESH_MIN_STANDARD;
}

uint8_t dashboard_refresh_minimum(bool is_bw, uint8_t max_partials)
{
    /* At one-minute sleeps, at least two partial cycles put the next regular
       full refresh no sooner than three minutes after the previous full. */
    if (is_bw &&
        max_partials >= DASH_REFRESH_BW_SHORT_INTERVAL_MIN_PARTIALS) {
        return DASH_REFRESH_MIN_BW_TWO_PARTIALS;
    }
    return DASH_REFRESH_MIN_STANDARD;
}

bool dashboard_refresh_config_is_valid(uint8_t refresh_min, bool is_bw,
                                       uint8_t max_partials)
{
    return refresh_min >= dashboard_refresh_minimum(is_bw, max_partials) &&
           refresh_min <= DASH_REFRESH_MAX;
}

dashboard_quiet_action_t dashboard_quiet_action(bool quiet_active,
                                                bool keep_wifi_connected,
                                                bool quiet_keep_connected)
{
    if (!quiet_active) return DASH_QUIET_INACTIVE;
    if (keep_wifi_connected && quiet_keep_connected) {
        return DASH_QUIET_PAUSE_CONNECTED;
    }
    return DASH_QUIET_DEEP_SLEEP;
}

bool offline_partial_refresh_allowed(uint8_t partial_count,
                                     uint8_t max_partials,
                                     uint16_t renders_since_full,
                                     uint16_t render_cap)
{
    return max_partials > 0 &&
           partial_count < max_partials &&
           renders_since_full < render_cap;
}
