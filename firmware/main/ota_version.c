#include "ota_version.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} ota_semver_t;

/*
 * Parse one numeric component in place: 1+ ASCII digits, no leading zero unless
 * the component is exactly "0". On success advances *p past the digits and
 * writes the value. Returns false on an empty component, a leading zero, or a
 * value that would overflow uint32_t.
 */
static bool parse_component(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (*s < '0' || *s > '9') return false;
    /* Reject "00", "01", ... but accept a lone "0". */
    if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') return false;

    uint32_t val = 0;
    while (*s >= '0' && *s <= '9') {
        uint32_t digit = (uint32_t)(*s - '0');
        if (val > (UINT32_MAX - digit) / 10u) return false; /* overflow */
        val = val * 10u + digit;
        s++;
    }
    *p = s;
    *out = val;
    return true;
}

/*
 * Parse "vMAJOR.MINOR.PATCH" into `out`.
 *  - require_v: when true the leading 'v' is mandatory (manifest `latest`);
 *    when false it is tolerated as optional (locally embedded `running`).
 *  - allow_suffix: when true a trailing "-<suffix>" after PATCH is accepted
 *    (a `git describe` running version); when false PATCH must end the string.
 */
static bool parse_semver(const char *s, bool require_v, bool allow_suffix,
                         ota_semver_t *out)
{
    if (!s) return false;
    if (s[0] == 'v') {
        s++;
    } else if (require_v) {
        return false;
    }

    const char *p = s;
    if (!parse_component(&p, &out->major)) return false;
    if (*p != '.') return false;
    p++;
    if (!parse_component(&p, &out->minor)) return false;
    if (*p != '.') return false;
    p++;
    if (!parse_component(&p, &out->patch)) return false;

    if (*p == '\0') return true;
    if (allow_suffix && *p == '-') return true;
    return false;
}

bool ota_download_url_is_canonical(const char *url, const char *latest_version)
{
    if (!url || !latest_version) return false;

    char expected[256];
    int n = snprintf(expected, sizeof(expected),
                     "https://github.com/%s/releases/download/%s/%s",
                     OTA_REPO_SLUG, latest_version, OTA_ASSET_NAME);
    if (n < 0 || (size_t)n >= sizeof(expected)) return false;

    return strcmp(url, expected) == 0;
}

bool ota_version_is_newer(const char *latest, const char *running)
{
    ota_semver_t l, r;
    /* `latest` must be strictly canonical; `running` may omit 'v' and carry a
     * git-describe suffix. Either parse failure fails closed (no install). */
    if (!parse_semver(latest, /*require_v=*/true, /*allow_suffix=*/false, &l)) {
        return false;
    }
    if (!parse_semver(running, /*require_v=*/false, /*allow_suffix=*/true, &r)) {
        return false;
    }

    if (l.major != r.major) return l.major > r.major;
    if (l.minor != r.minor) return l.minor > r.minor;
    return l.patch > r.patch;
}
