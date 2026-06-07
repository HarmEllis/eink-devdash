# Plan: OTA manifest over the Cloudflare relay

## Goal

Make firmware OTA work for relay-based API profiles, not just local/self-hosted
HTTPS APIs. Today the firmware fetches `<base>/ota/manifest`; over a relay that
resolves to `/d/<uuid>/ota/manifest`, which the Worker does not route, so the
check 404s. We add a dedicated manifest route the relay serves from a manifest
the API publishes over the existing outbound WebSocket — and we harden the
firmware trust model so the global `RELAY_PUBLISH_KEY` cannot become a
remote-code-install credential.

Chosen approach: **separate manifest route** (keeps the relay a payload-agnostic
store-and-forward and leaves the firmware OTA *fetch* flow identical between
local and relay transports). Downgrade policy: **upgrade-only** (firmware
installs strictly newer versions; equal/older are skipped).

## Security model (drives the firmware changes)

`RELAY_PUBLISH_KEY` is a *global* Worker publisher credential (see AGENTS.md),
not a per-device secret. The relay only validates that a published payload is an
object, and today the firmware accepts any download URL whose host is
`github.com`/`objects.githubusercontent.com`. So a leaked publish key could
advertise a valid-but-malicious ESP image from another GitHub repo, or pin an
old vulnerable *official* release to force a downgrade. The image SHA is an
integrity check, not an authenticity check. We therefore move the trust anchor
into the firmware:

1. **Canonical URL pinning.** Hard-code the repo slug + asset name in firmware
   (mirroring `api/src/routes/ota.ts`) and accept a `downloadUrl` only if it is
   exactly `https://github.com/HarmEllis/eink-devdash/releases/download/<latestVersion>/eink-devdash.bin`.
   The URL's version segment must equal `latestVersion`.
2. **Upgrade-only.** Install only when `latestVersion` is strictly newer than
   the running version (ordering-aware compare, not equality). Equal or older →
   skip. Closes downgrade attacks even for a freshly published old release.
3. **Manifest freshness (relay).** TTL on the stored manifest so a stale cached
   manifest can't act as a replay (see relay changes). The TTL uses fixed,
   independent constants on each side — it is **not** derived from the
   user-configurable dashboard publish interval.

### Freshness constants (no cross-config dependency)

- API: a non-configurable `MANIFEST_HEARTBEAT_MS = 60_000` in `relay-client.ts`.
  The manifest is (re)published on its own timer at this cadence, independent of
  `RELAY_PUBLISH_INTERVAL_MS` (which only governs the dashboard).
- Worker: a hard-coded `MANIFEST_TTL_MS = 300_000` (5 min), with a code comment
  asserting the invariant `MANIFEST_TTL_MS >= 3 * API manifest heartbeat`. Both
  constants are fixed in code; neither reads the other's config, so a user
  changing the dashboard interval cannot open a 404 window.

## Non-goals

- No change to where firmware binaries live (still GitHub Releases).
- No OTA rollback via downgrade (explicitly dropped by the upgrade-only policy).

## Changes

### Firmware — `firmware/main/ota_client.c`

1. Keep the cert-bundle attach on the manifest fetch and the 404 → no-op
   fallback (404 = manifest not published / older relay / past TTL).
2. Add hard-coded `OTA_REPO_SLUG "HarmEllis/eink-devdash"` and
   `OTA_ASSET_NAME "eink-devdash.bin"`. Add a pure helper
   `ota_download_url_is_canonical(const char *url, const char *latest_version)`
   that builds the expected canonical URL
   (`https://github.com/<slug>/releases/download/<latest_version>/<asset>`) and
   compares it exactly to `url`. Reject in the caller before
   `download_and_install`.
   - This canonical check **subsumes** the initial-URL host check: the only
     accepted initial URL is on `github.com`. `download_url_host_allowed` may
     stay as a cheap early guard, but the plan no longer claims it validates the
     302 redirect target — see the redirect-trust note below.
3. **Upgrade-only version policy with an exact grammar.** Replace the equality
   `versions_match` gate with `ota_version_is_newer(const char *latest,
   const char *running)`. Fail closed: any parse failure returns `false`
   (no install).
   - `latest` (from the manifest) MUST be canonical `vMAJOR.MINOR.PATCH` with a
     **required** leading `v`, exactly three dot-separated components, each 1+
     ASCII digits with no leading zero (except a lone `0`), no trailing
     characters. The `v` is mandatory because release tags carry it and the
     download URL embeds the version verbatim (`/releases/download/v0.4.0/...`);
     an unprefixed `0.4.0` would point at a non-existent tag.
   - `running` (from `esp_app_get_description()->version`, a `git describe`
     string like `v0.3.1-2-gabc-dirty`) is parsed by the same three-component
     grammar for its **prefix**, tolerating a missing leading `v` (only this
     locally embedded value may omit it) and a trailing `-<suffix>` after PATCH.
     Anything else fails closed.
   - Components are parsed as bounded `uint32_t` with explicit overflow
     detection (reject a component that would overflow). Compare numerically:
     major, then minor, then patch. Install only when `latest` is strictly
     greater. A `git describe` suffix on an equal base (e.g. running
     `v0.3.1-2-gabc` vs latest `v0.3.1`) is **not** newer → no install.
4. Extract `ota_download_url_is_canonical` and `ota_version_is_newer` (plus the
   `vX.Y.Z` parser) into a pure source/header pair with no ESP-IDF HTTP/TLS
   dependencies, so the host unit test can compile them directly (see Tests).

### Relay — `relay/src/index.ts`

1. Add a `manifest` frame: `{ type: 'manifest'; payload: Record<string, unknown> }`.
2. `webSocketMessage`: validate as object/non-array; store via
   `storeManifest(payload)` → `storage.put('manifest', { payload, lastPublishAt })`;
   reply `ack`.
3. Router regex → also match `/d/<uuid>/ota/manifest`:
   `/^\/d\/([^/]+)(?:\/dashboard|\/ota\/manifest)?$/`; reuse `dashboardRequest`
   (same device-token auth + uuid header injection).
4. `DashboardRelay.fetch`: when `url.pathname.endsWith('/ota/manifest')`, return
   the stored manifest payload **only if fresh**: if
   `Date.now() - lastPublishAt > MANIFEST_TTL_MS` (the fixed 300_000 constant
   above), return the graceful `404 { error: 'Manifest stale' }`; absent →
   `404 { error: 'Manifest not published yet' }`.
5. Track `lastManifestPublishAt` for `/admin/stats`.

### Redirect-trust note (firmware)

`esp_https_ota()` follows the GitHub 302 to `objects.githubusercontent.com`
internally; the existing `download_url_host_allowed()` only inspects the initial
URL, so it does not validate per-hop redirect targets. We deliberately rely on
the canonical `github.com` initial URL plus TLS (cert bundle covers both hosts)
for redirect trust, rather than implementing per-redirect validation. The plan
text and ADR will say this explicitly instead of implying the allow-list guards
the redirect.

### API — `api/src/routes/ota.ts`

1. Export `getOtaManifest(): OtaManifest` as the single source of truth; route
   handler uses it.
2. **Validate `APP_VERSION` before advertising it.** `buildManifest` (or
   `readOtaEnv`) accepts `appVersion` only if it matches canonical
   `^v\d+\.\d+\.\d+$` — leading `v` **required** (matching release tags and
   `buildDownloadUrl`), each component a safe integer (no leading zero except
   `0`, within `2^32-1` so the firmware parser never overflows). A malformed,
   unprefixed, or out-of-range `APP_VERSION` yields `{ otaEnabled: false }` and
   logs a warning — the firmware never receives a version it would reject.

### API — `api/src/relay/relay-client.ts`

1. Add `getManifest?: () => Promise<OtaManifest> | OtaManifest` to options.
2. Add a standalone, guarded `publishCurrentManifest()` (own `try/catch`,
   re-check `ws.readyState === OPEN` after the await, log on failure) —
   independent of `publishCurrentDashboard()`.
3. Invoke `publishCurrentManifest()` on `open` (not chained after the dashboard
   publish) and on its **own** fixed `MANIFEST_HEARTBEAT_MS` (60_000) timer —
   not the dashboard interval — so the relay's `MANIFEST_TTL_MS` (300_000) is
   never reached during normal operation and a relay upgrade that dropped the
   first frame recovers within one heartbeat. Clear the timer on close/stop.

### API — wiring (`api/src/index.ts`)

1. Pass `getManifest: () => getOtaManifest()` into `createRelayPublisher`.

### Docs

1. `docs/decisions/0005-ota-updates.md`: replace the (erroneous) "relay profiles
   do not provide OTA" paragraph with the real design: API publishes the
   manifest over the outbound WS; relay serves `/d/<uuid>/ota/manifest` with
   device-token auth + TTL; firmware pins the canonical URL and is upgrade-only;
   404 is a graceful skip.
2. `README.md`: update the relay diagram, route/profile table, and operating
   notes; document that `RELAY_PUBLISH_KEY` can now also select an OTA release,
   and the canonical-URL + upgrade-only constraints that bound its blast radius.

## Tests

- `relay/test/relay.test.ts`: manifest stored on frame; `GET /d/<uuid>/ota/manifest`
  returns it with a valid token; 404 before publish; 401 with wrong token;
  reconnect re-publishes; relay-upgrade case where an earlier manifest frame was
  rejected/absent still recovers on next publish. **Fake-clock TTL boundary
  tests:** manifest stays available across normal heartbeating (just inside
  `MANIFEST_TTL_MS`), and returns 404 once heartbeats stop and the age exceeds
  `MANIFEST_TTL_MS`.
- `api/src/relay/relay-client.test.ts`: manifest frame sent on open and on the
  fixed heartbeat timer (fake clock); provider rejection is caught and logged;
  socket closing between `getManifest()` resolution and `send()` is handled;
  timer cleared on stop.
- `api/src/routes/ota.test.ts`: route serves via `getOtaManifest`; a malformed,
  overflowing, suffixed, or **unprefixed** (`0.4.0`) `APP_VERSION` yields
  `{ otaEnabled: false }`; a canonical `v0.4.0` yields the enabled manifest.
- Firmware pure helpers, via a host unit-test target (see Verification):
  - `ota_download_url_is_canonical`: reject another repo, another asset name,
    a version/URL mismatch, a non-https scheme; accept the exact canonical URL.
  - `ota_version_is_newer`: accept strictly newer; reject equal, older, a
    `git describe`-suffixed equal base; the 9→10 transition (`v0.9.0` <
    `v0.10.0`, proving numeric not lexical compare); fail closed on malformed,
    on an **unprefixed** `latest` (`0.4.0`), and on an overflowing component
    (e.g. `v4294967296.0.0`). The tolerant-`v` path applies only to `running`.
  - The helpers live in a header/source pair with no ESP-IDF HTTP/TLS deps so
    the host test compiles them directly.

## Verification

All firmware commands run inside the devcontainer as the `node` user (per
AGENTS.md), never on the host:

- Relay: `docker exec -u node optimistic_hermann bash -c "cd /workspaces/eink-devdash/relay && npm test"`
- API: `docker exec -u node optimistic_hermann bash -c "cd /workspaces/eink-devdash/api && npm test"`
- Firmware build:
  `docker exec -u node optimistic_hermann bash -c "source /etc/profile.d/esp-idf.sh && cd /workspaces/eink-devdash/firmware && idf.py build"`
- Firmware host helper-tests: a dedicated host target under `firmware/test/`
  (IDF `linux` target with Unity) built and run in the devcontainer, e.g.
  `docker exec -u node optimistic_hermann bash -c "source /etc/profile.d/esp-idf.sh && cd /workspaces/eink-devdash/firmware/test && idf.py --preview set-target linux && idf.py build && idf.py monitor"`
  (exact invocation finalized when the target is added; it must run the
  `ota_download_url_is_canonical` / `ota_version_is_newer` cases above).
- Manual: a relay profile picks up `otaEnabled`, upgrades on a newer version,
  ignores equal/older, and a non-canonical `downloadUrl` is refused.

## Risk / rollback

- Firmware hardening is stricter than before: a malformed/old manifest is now
  skipped rather than installed. Old firmware + un-upgraded relay degrade to the
  404 → no-op path. Relay/api changes are additive; rollback is reverting them.
