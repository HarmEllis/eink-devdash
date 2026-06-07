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
