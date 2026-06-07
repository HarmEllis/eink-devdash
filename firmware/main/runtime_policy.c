#include "runtime_policy.h"

#include <string.h>

bool clock_should_apply(const char *iso, bool stale)
{
    return iso != NULL && iso[0] != '\0' && !stale;
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
    /* At one-minute sleeps, two partial cycles put the next regular full
       refresh three minutes after the previous full refresh. */
    if (is_bw &&
        max_partials == DASH_REFRESH_BW_SHORT_INTERVAL_PARTIALS) {
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
