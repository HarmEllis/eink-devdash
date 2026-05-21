# Codebase Review — 2026-05-21

- **Scope:** full repo at `main` (a8ede8c). No open PR.
- **Reviewer:** Claude (Opus 4.7), prompted by maintainer.
- **Coverage:** firmware (`firmware/`), API server (`api/`), flash server
  (`flash-server/`). No tests exist, so test coverage was not audited.

## Overview

E-ink developer dashboard on ESP32-S3 + WeAct 2.9" Black/Red, fronted by a
Node/Fastify API that aggregates GitHub, Claude, and Codex usage. ~5 kLOC
C firmware + ~700 LOC TypeScript. Architecture is well-considered:
deep-sleep duty cycle, NVS-backed configurable multi-network/multi-API
config, captive portal provisioning, retained partial-refresh state in
RTC memory, and a recently added red-free BW partial path (ADR-0003).

Code reads cleanly throughout — naming, layering, and the per-file
comments explaining *why* are above average for a hobby firmware repo.

## Bugs (real)

### B1. `html_escape` size bug in `render_api` — `firmware/main/wifi_prov.c:520`

```c
char *url_esc = malloc(DASH_API_URL_MAX * 6 + 1);
...
html_escape(... , url_esc, sizeof(url_esc));   // sizeof(char*) == 4
```

`sizeof(url_esc)` is the size of the pointer (4 bytes on Xtensa), not the
allocation. `html_escape`'s loop guard `out + 6 < dst_sz` is immediately
false, so `url_esc` ends up `""` every time.

**Result:** the API URL is never pre-filled in the portal form, even for
saved profiles — the user has to retype the URL on every edit.

**Fix:**

```c
const size_t url_esc_sz = DASH_API_URL_MAX * 6 + 1;
char *url_esc = malloc(url_esc_sz);
...
html_escape(..., url_esc, url_esc_sz);
```

The sibling call in `render_network` uses a stack buffer with real
`sizeof`, so SSID escape is fine.

### B2. Fastify auth hook doesn't return — `api/src/index.ts:16-22`

```ts
app.addHook('onRequest', async (req, reply) => {
  if (req.url === '/health') return
  const auth = req.headers.authorization
  if (!auth || auth !== `Bearer ${DEVICE_TOKEN}`) {
    reply.code(401).send({ error: 'Unauthorized' })
  }
})
```

`reply.send` in an async hook works because Fastify detects the reply is
sent, but the contract is fuzzy. Make it explicit:

```ts
return reply.code(401).send({ error: 'Unauthorized' })
```

Otherwise refactors that touch the handler chain can accidentally
double-send.

### B3. Truncated API response renders as zeros — `firmware/main/api_client.c:187-190`

`fetch_one` caps the response at 4 KiB and sets `ctx.truncated`. The
flag is logged but not surfaced to the caller, so a half-parsed JSON
proceeds to `cJSON_Parse` and may silently render zeros on the dashboard.

**Fix:** return `ESP_FAIL` (and set `out->offline = true`) when
`ctx.truncated` is set, or bump `RESPONSE_BUF_SIZE` to match the worst-
case API payload.

## Code quality / style

- **`api_client.c:231-260` failover loop is hard to read.** The
  `(pass == 0 && start >= 0) ? start : i` trick plus the
  `continue`/`break` rules work, but a flatter "try `start`, then
  iterate skipping `start`" is shorter and easier to verify. Same
  behavior, half the cognitive load.
- **`CMD_DISP_UPDATE_CTRL` naming**, `eink_weact29.c:15`. The SSD1680
  calls 0x22 "Display Update Control 2". Renaming to
  `CMD_DISP_UPDATE_CTRL2` makes the pair (`_CTRL1` / `_CTRL2`) symmetric
  and matches the datasheet.
- **`parse_form_body` recomputes `strlen(body)` each iteration**,
  `wifi_prov.c:279`. Cache it once outside the loop; with 24 KiB max
  bodies this is wasteful.
- **`storage_validate_api_url`** rejects `?`, `&`, `=`, `#`, `%`, `~` —
  so URLs with query strings or auth-tokens-in-URL are silently invalid.
  Intentional? Worth a one-line comment if so; otherwise widen the
  allow-list.
- **`display.c` is 1146 lines** and mixes pixel primitives, font, icons,
  dashboard layout, and refresh-policy state machine. The refresh-
  decision block in `display_render` (lines 778-857) has enough flags
  (`s_first_refresh_done`, `s_last_red_state`, `s_last_bw_valid`,
  `s_last_content_valid`, `previous_frame_is_content`, `has_bw_diff`,
  `s_bw_fast_cycle_count`) to deserve its own helper that returns an
  enum {`SKIP`, `BW_PARTIAL`, `BW_FAST`, `FULL_COLOR`} — would also make
  the partial-refresh policy testable on host.

## Performance

- **`find_bw_diff_rect`** scans the full 4736-byte buffer on every
  render. Fine — ~0.3 ms — flagging only because it runs even when
  partial is rejected.
- **`render_portal_page` chunks aggressively**, good — but emits a
  ~3 KB inline `<style>` block on every GET. Serve it once via
  `/style.css` with a long `Cache-Control` and save ~3 KB per portal
  hit. Not load-bearing on a 1-user portal, but cheap.
- **SPI at 4 MHz**, `eink_weact29.c:249`. SSD1680 spec allows 20 MHz;
  conservative is fine but a quick bump to 10 MHz roughly halves the
  framebuffer write portion of the refresh (~100 ms saved per full
  refresh).

## Security

- **Single shared `DEVICE_TOKEN`** across all devices/networks
  (`api/src/index.ts:8`). For LAN-only this is OK, but the firmware
  happily talks to APIs over plain HTTP. If you ever expose the API
  beyond LAN, per-device tokens + TLS pinning would be next. Worth
  documenting the threat model in `docs/`.
- **Captive portal HTTP only**, no auth on `/save` — anyone on the
  SoftAP can write config. Mitigated by the WPA2 PoP gate on the AP
  itself. Documented in ADR-0002. Fine.
- **AP PoP isn't logged**, `wifi_prov.c:1091-1094`. Good — that was an
  easy mistake to make.

## Test coverage

Zero automated tests. For a firmware-heavy hobby project that's
defensible, but two parts are pure functions that would benefit from
host-side unit tests:

- `firmware/main/storage.c` — CRC, normalize, validate
- `firmware/main/wifi_prov.c` — `form_decode`, `parse_form_body`,
  `apply_form_to_cfg`

ESP-IDF's host-target build or a small standalone CMake target against
the host toolchain would catch the `sizeof(url_esc)` bug class
instantly.

API side has no tests either — `getCodexUsage` parsing in
`codex.service.ts` is the most fragile thing in the repo (4 different
JSON shapes) and screams for a Vitest table-test.

## Conventions / repo hygiene

- **English-only policy in AGENTS.md** is consistently applied across
  firmware and docs. Good.
- **Working-copy drift at review time**: `display.c`,
  `eink_weact29.{c,h}` were modified plus `docs/decisions/0003-…`
  untracked. ADR-0003 marked "Proposed" but the BW-partial code is
  already merged-style in the working tree — bump to "Accepted" when
  committing, and reference the commit hash from the ADR.
- **`firmware/managed_components/espressif__qrcode`** is committed as
  the only managed component. Either commit the whole
  `managed_components/` lock for reproducibility or remove it from git.

## Suggested priorities

1. Fix B1 (`sizeof(url_esc)`) — single character of behavioral impact,
   but every user editing an existing API entry hits it.
2. Fix B2 (Fastify 401 hook return).
3. Fix B3 (truncated response silently renders zeros).
4. Add a host-side unit test for `form_decode` + `apply_form_to_cfg` —
   would have caught B1 and protects the form contract going forward.
5. Mark ADR-0003 Accepted when committing.

No blockers to ship as-is, but B1 is worth a quick patch before the next
flash session.
