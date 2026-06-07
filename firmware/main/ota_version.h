#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure (no ESP-IDF HTTP/TLS dependency) helpers that form the firmware OTA
 * trust anchor. They are deliberately kept free of esp_* includes so the host
 * unit test under firmware/test/ can compile and exercise them directly.
 *
 * Why these live in firmware, not just the API: RELAY_PUBLISH_KEY is a global
 * Worker publisher credential (see AGENTS.md), not a per-device secret, and the
 * relay only checks that a published manifest is an object. A leaked key could
 * therefore advertise a valid-but-malicious image from another GitHub repo, or
 * pin an old vulnerable official release to force a downgrade. The image SHA is
 * an integrity check, not an authenticity check, so the device must anchor
 * trust itself: pin the canonical download URL and install upgrades only.
 */

/* Canonical firmware release source, mirrored from api/src/routes/ota.ts.
 * Hard-coded on purpose so a wrong fork or a leaked publish key cannot point
 * the device at someone else's binaries. */
#define OTA_REPO_SLUG  "HarmEllis/eink-devdash"
#define OTA_ASSET_NAME "eink-devdash.bin"

/*
 * True iff `url` is byte-for-byte the canonical GitHub Releases download URL
 * for `latest_version`:
 *
 *   https://github.com/<OTA_REPO_SLUG>/releases/download/<latest_version>/<OTA_ASSET_NAME>
 *
 * This is an exact string comparison, not a host/scheme allow-list, so it
 * subsumes any "is this on github.com over https" check: the only URL it
 * accepts already starts with https://github.com/. `latest_version` is
 * embedded verbatim, so a mismatched version segment is rejected too. Returns
 * false on NULL inputs.
 */
bool ota_download_url_is_canonical(const char *url, const char *latest_version);

/*
 * True iff `latest` is a strictly newer release than `running` (upgrade-only).
 *
 *  - `latest` (from the manifest) MUST be canonical "vMAJOR.MINOR.PATCH": a
 *    required leading 'v', exactly three dot-separated components, each 1+
 *    ASCII digits with no leading zero (except a lone "0"), and no trailing
 *    characters. The 'v' is mandatory because release tags carry it and the
 *    download URL embeds the version verbatim (/releases/download/v0.4.0/...).
 *  - `running` (from esp_app_get_description()->version, a `git describe`
 *    string like "v0.3.1-2-gabc-dirty") is parsed by the same grammar for its
 *    prefix, tolerating a missing leading 'v' (only this locally embedded value
 *    may omit it) and a trailing "-<suffix>" after PATCH.
 *
 * Components are bounded uint32_t with explicit overflow detection. Comparison
 * is numeric (major, then minor, then patch), so v0.9.0 < v0.10.0. A
 * git-describe suffix on an equal base (running v0.3.1-2-gabc vs latest
 * v0.3.1) is NOT newer. Fails closed: any parse failure or overflow returns
 * false (no install).
 */
bool ota_version_is_newer(const char *latest, const char *running);
