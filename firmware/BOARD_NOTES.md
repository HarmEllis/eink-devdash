# Board Notes — ESP32-S3 Super Mini (nologo.tech)

Hardware-specific quirks and verified behavior for the exact module in use.

## Wake source: GPIO0 (BOOT button)

**Verified on:** 2026-05-17 with `firmware/main/main_bringup.c`.

The BOOT button on the Super Mini is wired to **`GPIO_NUM_0`** and is the
only physical button available for waking the chip from deep sleep (the
RESET button performs a hard reset, which is observable as
`ESP_SLEEP_WAKEUP_UNDEFINED` and clears RTC memory).

`GPIO0` is also a strapping pin: it must be high at reset. The internal
pull-up keeps it high in active mode, and the BOOT button pulls it to GND
when pressed.

### Configuration that works

Two pull-up steps are required because they live in different power
domains on the ESP32-S3:

```c
/* Active mode — digital IO domain, used while the chip is awake. */
gpio_config_t io = {
    .pin_bit_mask = 1ULL << GPIO_NUM_0,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
gpio_config(&io);

/* Deep-sleep — RTC IO domain, used while the digital IO domain is off. */
rtc_gpio_init(GPIO_NUM_0);
rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
rtc_gpio_pullup_en(GPIO_NUM_0);
rtc_gpio_pulldown_dis(GPIO_NUM_0);

esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);   /* wake on level 0 */
esp_sleep_enable_timer_wakeup(timer_us);       /* fallback timer  */
```

**Why both pulls:** during deep sleep the digital IO domain is powered
off, so a pull-up set via `gpio_config` is gone. The pin floats, ext0
fires immediately (level 0), and the chip enters a wake/sleep boot loop.
Configuring the pull-up through `rtc_gpio_*` keeps it asserted by the
RTC domain, which stays powered.

### Observed behavior

| Observation | Result |
|-------------|--------|
| Idle GPIO0 level in active mode | `1` (pull-up holds it high) |
| Button-press level | `0`, clean transition, no visible bounce |
| BOOT pressed during deep sleep | Chip wakes within ~1 s, app_main re-runs |
| No input during deep sleep | Chip stays asleep until the timer fallback fires (confirmed indirectly: no spurious wakes / boot loop) |

### Pins used elsewhere (do not reuse for wake)

- `GPIO9` — e-ink D/C
- `GPIO10` — e-ink CS
- `GPIO11` — SPI MOSI
- `GPIO12` — SPI SCK
- `GPIO13` — e-ink BUSY
- `GPIO1`  — e-ink RST
- `GPIO48` — onboard LED

## USB-Serial-JTAG behavior

The Super Mini has no external USB-UART bridge; it uses the ESP32-S3's
built-in USB-Serial-JTAG. Implications:

- Improv and browser Web Serial traffic must use USB-Serial-JTAG, not
  UART0. UART0 is only useful if a separate wired debug adapter is attached.
- `esp_deep_sleep_start()` powers down the USB-CDC controller. The host
  COM port disappears for the entire sleep duration.
- After wake, the host needs ~0.5-1.5 s to re-enumerate the USB device,
  and any logs written during that window are silently dropped.
- Browser-based Web Serial monitors (e.g. the flash-server's terminal)
  require a manual "Select port" click to reattach after re-enumeration
  — they will not reconnect automatically.
- Opening the USB-Serial-JTAG CDC port for plain serial I/O should not reset
  the chip by itself. A boot log line such as
  `rst:0x15 (USB_UART_CHIP_RESET)` means the host sent the USB-Serial-JTAG
  reset control sequence, normally via DTR/RTS transitions used by esptool,
  ESP Web Tools, or a terminal that asserts modem-control lines on open.
  ESP-IDF v5.3 has `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`, but that only
  prevents automatic light sleep while the USB-Serial-JTAG port is connected;
  it does not disable the reset/download control sequence.

For development this is workable but not pleasant; for production it is
irrelevant because the chip is battery-powered and the host is absent.
When iterating on sleep-related code, either:

1. Use a serial monitor that auto-reconnects (`idf.py monitor`,
   `screen /dev/ttyACM0 115200`, `tio`, etc.) on a Linux/macOS host
   that has direct `/dev/ttyACM*` access, or
2. Persist diagnostic state in RTC slow memory (`RTC_DATA_ATTR`) so the
   next reachable boot can re-emit the missed history. The bring-up
   firmware used this technique.

## Cold boot vs reset vs wake (RTC memory semantics)

- **Cold boot** (power applied): all RTC memory zero-initialised.
  `esp_sleep_get_wakeup_cause()` returns `UNDEFINED`.
- **RESET button**: same as cold boot for RTC memory — values reset.
  `esp_sleep_get_wakeup_cause()` returns `UNDEFINED`.
- **Wake from deep sleep**: RTC slow memory (`RTC_DATA_ATTR`) is
  preserved; `RTC_NOINIT_ATTR` is preserved without zero-init. The wake
  cause reflects the source (`EXT0`, `TIMER`, etc.).

## V4 provisioning contract (SoftAP HTTP portal)

The on-device captive portal — served from `wifi_prov.c` at
`http://192.168.4.1/` while the AP is up — pre-renders all 5 networks × 5
APIs slots so the form is usable with JavaScript disabled. The C handler
parses `application/x-www-form-urlencoded` body fields by positional
prefix. Field names match `design_handoff_eink_v4_provisioning/README.md`:

| Field            | Type     | Meaning                                          |
|------------------|----------|--------------------------------------------------|
| `wN_i`           | hidden   | Profile id of network N (0 for empty slots).     |
| `wN_on`          | checkbox | Network N enabled. Off ⇒ remove on save.         |
| `wN_ssid`        | text     | SSID (1..32 chars).                              |
| `wN_pass`        | password | New password. Empty + clearpw off ⇒ keep saved.  |
| `wN_clearpw`     | checkbox | Force-erase saved password.                      |
| `wN_aK_i`        | hidden   | Profile id of API K under network N.             |
| `wN_aK_on`       | checkbox | API K enabled.                                   |
| `wN_aK_url`      | url      | `http://` only — TLS is out of scope.            |
| `wN_aK_tok`      | password | New token. Same keep/clear/replace rules as PASS.|
| `wN_aK_cleartok` | checkbox | Force-erase saved token.                         |
| `iv`             | number   | Refresh interval, 3..60 (stored in cfg.refresh_min). |

`N ∈ 0..4`, `K ∈ 0..4`. Validation lives in `validate_network` and mirrors
`storage_validate_api_url`. Body cap is 24 KB — anything larger gets 413.
`POST /save` returns the V4 saved page and schedules `esp_restart()` 4 s
later via `esp_timer`, leaving time for the on-page spinner→check
transition before the AP drops.

The HTTP server runs with `uri_match_fn = httpd_uri_match_wildcard`. The
explicit captive-detect routes (`/generate_204`, `/hotspot-detect.html`,
`/ncsi.txt`, `/connecttest.txt`) and the catch-all `/*` reply 302 to
`http://192.168.4.1/`. The wildcard must be registered last so the more
specific URIs match first.

The captive DNS responder (`captive_dns.c`) binds UDP/53 to the AP netif
only and answers every A query with `192.168.4.1`. AAAA / HTTPS / SVCB /
unknown types get NOERROR + ANCOUNT=0 — never a timeout — so iOS and
Android pop the captive sheet promptly.

The on-screen QR encodes the standard WiFi join string
(`WIFI:T:WPA;S:devdash-XXXX;P:<12-char alnum>;;`). The AP password is
generated once at first boot via `storage_get_or_init_ap_password` and
persisted under NVS key `ap_pwd` in the `devdash` namespace. Factory
reset (`storage_erase`) clears the key; the next boot generates a new
password. ADR-0002 records the policy.

## Phase 0 — WeAct 2.9" BW panel + per-region partial refresh bring-up

The driver paths and refresh modes that support the second panel
(`EINK_PANEL_WEACT_29_BW`) plus the SAFE_BW recovery surface
(`EINK_REFRESH_SAFE_BW`) are validated on a throwaway harness branch
(`phase0/harness-bw-29`) before the Phase 1 feature branch is merged.

### Harness usage

Flash a phase0 harness build by enabling
`CONFIG_DEVDASH_PHASE0_HARNESS=y` in menuconfig. Pick one panel
(`CONFIG_DEVDASH_PHASE0_PANEL_BWR` / `_BW`) and one scenario
(`CONFIG_DEVDASH_PHASE0_TEST_GATE_A`,
`_GATE_B_CLEARED`, `_GATE_B_RED`, or `_GATE_B_WRONG_VARIANT`).
The harness boots, runs the scenario once, then idles. Press EN/RST or
power-cycle to rerun. Re-flash to switch scenarios.

### Gate 0.A — narrow-X partial windows on BW

Pass criteria (all must hold):
- Pixels inside each narrow-X partial window update cleanly.
- Pixels outside the window do not change.
- After 10 consecutive narrow-X partials followed by `EINK_REFRESH_BW_FULL`,
  no permanent ghost stripes remain.

Branch decision:
- **PASS** → Phase 1 removes the full-row clamp in `find_bw_diff_rect()`
  (`rect->x = 0; rect->w = EINK_WIDTH;` in `firmware/main/display.c`) under
  `effective_panel_variant() == BW` and ships the per-region table as
  designed (Layout A: 6 regions, Layout B: 3 regions).
- **FAIL** → Phase 1 keeps the clamp and falls back to two full-row Y-band
  regions (`band_left`, `band_right`).

Result: _`Narrow-X partial bring-up — 2026-06-02 — PASS on WeAct 2.9 BW`._
The first run failed before the BW V2 partial LUT fix: the harness used native
BW geometry (`RAMX=1..16`) and drew the baseline `BW_FULL` stripes
successfully, but the first narrow-X partial window (`x=64 y=80 w=32 h=80`)
timed out waiting for BUSY release after activation. See
`.temp/esp-web-tools-logs(0gatea).txt`.
After loading the BW V2 partial LUT (`0x32`, 151 bytes) and using update
trigger `0xCC`, the repeat run completed all 10 narrow-X partial windows
without BUSY timeout. See `.temp/esp-web-tools-logs(0gatea2).txt` and
`.temp/eink_bw_0phasea2.1.JPEG` / `.temp/eink_bw_0phasea2.2.JPEG`.
The photos show expected partial-refresh ghosting from the baseline stripes,
but no visible random pixels or corruption outside the requested windows. The
harness intentionally finishes with `fill_white()` plus `EINK_REFRESH_BW_FULL`,
so a blank panel after completion is the expected final state.

### Gate 0.B — SAFE_BW QR readability across panels and recovery

Pass criteria (all must hold for every scenario below):
- A QR-like pattern rendered via `EINK_REFRESH_SAFE_BW` is scannable.
- On BWR, no red residue / pigment / ghost remains anywhere on the panel.
- The harness logs show `SAFE_BW reset: 0x21 = 0x00, 0x00 (mode-dispatched)`.
- On a known BW handle, the init log shows `RAMX=1..16`.
- On a known BW handle, `SAFE_BW` writes both mono RAM planes (`0x24` and
  `0x26`) so the old/base plane is not left with reset-time garbage.
- On BWR, `SAFE_BW` writes `CMD_WRITE_RED_RAM` (0x26) with an all-zero no-red
  plane so red RAM is not left with reset-time or previous-frame contents.

Scenarios:
- BW panel, `h.variant = BW` (native baseline).
- BWR panel, `h.variant = BWR`, cleared start (cleared FULL_COLOR then SAFE_BW).
- BWR panel, `h.variant = BWR`, red-preconditioned start (red-heavy
  FULL_COLOR poster then SAFE_BW), covering sub-cases
  (a) immediate, (c) software-reset entry, (d) power-cycle entry.
  Sub-case (b) — deep-sleep wake — is effectively identical to
  software-reset entry here because the SSD1680 enters deep sleep between
  renders.
- BW panel, `h.variant = BWR` (mismatched-variant simulation — the
  saved-BWR-config + swapped-in-BW-module recovery case).

Branch decision:
- **All PASS** → Phase 1 proceeds with the bootstrap / recovery design.
- **Any FAIL on BWR** → fall back to a real selectable bootstrap path
  (`CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT` + BOOT-button override + serial
  override). Update this section and `README.md` with the SKU-specific
  build matrix.

Result: _Partial — `Safe-mode bring-up — 2026-06-02 — PASS on WeAct 2.9 BW native cleared start`._
The Gate 0.B cleared scenario rendered cleanly on the BW panel after moving
the BW RAM X window to `RAMX=1..16`; the earlier noisy left byte-column was
gone in `.temp/eink_bw_0phaseb.JPEG` / user photo, and
`.temp/esp-web-tools-logs(0gateb3).txt` confirms `variant=BW, RAMX=1..16`.
`Safe-mode bring-up — 2026-06-02 — READABLE with geometry offset on WeAct 2.9 BW wrong-variant recovery`.
With `hardware=BW, h.variant=BWR`, the recovery frame rendered without the
noisy byte-column and remained readable, but it was visibly shifted because
the deliberate mismatch selected BWR geometry (`RAMX=0..15`) on BW glass
instead of the native BW `RAMX=1..16`. The log in
`.temp/esp-web-tools-logs(0gateb-wrong).txt` confirms the mismatch.
Remaining Gate 0.B scenarios still need per-panel results before marking the
gate fully passed: BWR red-preconditioned.
`Safe-mode bring-up — 2026-06-02 — PASS on WeAct 2.9 BWR cleared start`.
The native BWR cleared scenario rendered cleanly with no red/black noisy
byte-column. The log in `.temp/esp-web-tools-logs(bwr-0gateb-cleared).txt`
confirms `configured panel=BWR`, `variant=BWR`, and `RAMX=0..15`; the photo in
`.temp/eink_bwr_0phaseb3-cleared.JPEG` shows no red residue. Any run that
uses `RAMX=0..15` is visibly offset from a run that uses `RAMX=1..16`; this is
visible on the BW panel itself when comparing native BW (`RAMX=1..16`) against
wrong-variant BW recovery (`RAMX=0..15`). After rotation, the one-byte RAM X
offset maps to an 8 px landscape-axis shift.
`Safe-mode bring-up — 2026-06-02 — FAIL on WeAct 2.9 BWR red-preconditioned sub-case (a)`.
The log in `.temp/esp-web-tools-logs(bwr-0gateb-red-pc).txt` confirms the
native BWR red-preconditioned run (`configured panel=BWR`, `variant=BWR`,
`RAMX=0..15`), but `.temp/eink_bwr_0phaseb3-red-pc.JPEG` still shows a solid
red bar after the SAFE_BW refresh. The checker pattern remains readable, but
the red residue violates Gate 0.B.

### Gate 0.B branch outcome — selectable bootstrap fallback engaged

Because Gate 0.B **FAILED on BWR red-preconditioned**, the "Any FAIL on BWR"
branch applies: the panel-agnostic `SAFE_BW` bootstrap is **not** the recovery
path. Phase 1 (feature branch `feat/weact-bw-29-and-region-partials`) instead:

- Resolves the panel variant at every boot so it is **known before the first
  draw**: a real saved v3 config wins; otherwise the build-stamped SKU default
  `CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT` (Kconfig `int`, `range -1 1`,
  `default -1` so a SKU build fails closed if unset; the repo dev/CI build sets
  `0` = BWR in `sdkconfig.defaults`). `storage_default_panel_variant()` also
  seeds the storage defaults and the v2→v3 migration, so an upgraded BW-SKU
  device resolves to BW, not a hardcoded BWR.
- Renders provisioning / recovery surfaces (QR, connecting, wait, setup-failed,
  setup-timeout offline) through the **variant-aware** `display_full_refresh()`:
  `FULL_COLOR` on BWR (drives the red plane → clears prior red, fixing the red
  bar) and `BW_FULL` on BW. `EINK_REFRESH_SAFE_BW` / `display_full_refresh_safe()`
  are kept dormant as a build-stamped escape hatch but are no longer wired.
- Provides wrong-SKU recovery via a **cold-boot-only** override window (never on
  deep-sleep wakes, so battery timer wakes stay fast and the EXT0 BOOT gestures
  are unchanged): send `B` (BW) / `R` (BWR) over USB-Serial-JTAG within the first
  few seconds, or hold BOOT through the window to force BW for that one boot
  (one-boot only; the portal panel selector persists a choice).

SKU build matrix (also in `README.md`):

| SKU build      | `CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT` | Recovery if mis-flashed         |
|----------------|----------------------------------------|---------------------------------|
| WeAct 2.9" BWR | `0`                                    | cold-boot serial `B` / BOOT-hold → BW |
| WeAct 2.9" BW  | `1`                                    | cold-boot serial `R` → BWR            |
| repo dev / CI  | `0` (BWR, via `sdkconfig.defaults`)    | as above                              |

Gate 0.A (per-region BW partials) and the Gate 0.B fallback above are
implemented on the feature branch; on-hardware flash verification of the
per-region partials and the BWR recovery red-clear is the remaining pre-merge
step before the work lands on `main`.

Phase 1 implementation steps are not merged to `main` until both Gate 0.A
and Gate 0.B results are recorded above.
