# ADR-0002: SoftAP HTTP portal with WiFi QR and captive portal

- **Status:** Accepted
- **Date:** 2026-05-19
- **Branch where adopted:** `feat/provisioning-protocomm`
- **Supersedes:** the Improv-over-serial path on `main` (commits `9b4c201`,
  `1be103d`, `56969a0`); rolls in `feat/softap-provisioning` (`d8513a5`)
  via cherry-pick as the foundation.
- **Relates to:** [ADR-0001](0001-provisioning-transport.md) — the
  `wifi_prov_mgr` + protocomm + BLE + Security 2 direction, now
  deferred. This ADR replaces the active decision; ADR-0001 is kept as
  the audit trail for the alternative we may revisit later.

## Context

Provisioning needs to be easy enough that scanning a QR with a phone
camera leads straight into a working configuration screen, both for
first-time setup and for later edits (e.g. swapping API URL or
switching homes). Physical buttons (currently the BOOT button on
GPIO0, later potentially additional buttons) are the trigger to
re-enter the config window after the device is already provisioned.

The previous ADR (0001) chose a protocomm + Security 2 + BLE path
that is technically correct but heavy:

- No good off-the-shelf browser client for Security 2 with custom
  endpoints. We would either fork `esp-idf-provisioning-web` or roll
  our own SRP6a implementation.
- Salt/verifier key management adds first-boot and factory-reset
  complexity.
- BLE adds binary size (60-120 KB), a second SDK surface (NimBLE), and
  zero coverage on iOS Safari (no Web Bluetooth).
- For a hobbyist e-ink dashboard on a private LAN, the threat model
  does not justify SRP6a — the SoftAP password and physical proximity
  already gate access during the few-minute provisioning window.

A simpler path is already 80% built on the abandoned
`feat/softap-provisioning` branch (commit `d8513a5`): a custom
`esp_http_server` on a per-device SoftAP, with a form at
`http://192.168.4.1` writing into the existing `cfg_v2` storage. What
it lacks for the new vision is the soft UX: scan-to-join WiFi via QR
and automatic portal popup via captive portal.

## Decision

Provisioning is a **SoftAP HTTP portal** with three UX layers on top:

1. **WiFi QR on e-ink** in the standard `WIFI:T:WPA;S:devdash-<MAC>;P:<pwd>;;`
   format. iOS Camera and Android Camera/Google Lens recognise this
   and offer one-tap join.
2. **Captive portal**: a tiny DNS server on UDP/53 answers A queries
   with the SoftAP IP (`192.168.4.1`) and responds promptly with
   NODATA (RCODE=0, ANCOUNT=0) for unsupported types such as AAAA,
   SVCB, and HTTPS — never letting a query time out, since timeouts
   delay or skip the captive popup. The HTTP server serves the
   platform-specific captive-detection endpoints with a 302 redirect
   to the portal root, so iOS/Android/Windows pop the config page
   automatically once the phone joins the AP.
3. **BOOT button re-entry**: the existing GPIO0 ext0 deep-sleep wake
   already triggers `wifi_net_open_config_window()`. Same path covers
   later edits (e.g. changing the API URL). Future additional physical
   buttons can map to other modes without changing this flow.

The HTTP form supports the full `cfg_v2` model: up to 5 WiFi networks
× 5 API profiles per network, rendered server-side from C without a
template engine. The response is streamed via
`httpd_resp_send_chunk` (the full 5×5 form can exceed a small static
buffer), and the POST handler enforces an explicit body-size cap
with a 413 response on overflow.

Improv-over-serial is removed from the firmware tree. Web Serial
remains only as the path the flasher uses to flash the binary via
ESP Web Tools — not for runtime configuration.

## Why this and not the alternatives

### vs. `wifi_prov_mgr` + protocomm + Security 2 + BLE (ADR-0001)

Over-engineered for the threat model and lacks a good browser client.
Deferred until a use case (e.g. mobile-only provisioning over BLE on
Android, or single-cable Web Serial via TinyUSB CDC) actually
justifies the complexity.

### vs. Web Serial / Improv

Same hardware reason as ADR-0001: ESP32-S3 USB-Serial-JTAG resets the
chip on `port.open()` on the Super Mini (no external USB-UART chip).
Not solvable at the protocol layer.

### vs. mDNS-based provisioning

Requires the phone to be on the same network already — which is the
chicken-and-egg problem we are solving. mDNS is fine for the
post-provisioning API path (already handled by `bonjour-service` on
the server) but not for first-time setup.

## Consequences

- `feat/softap-provisioning` (`d8513a5`) is cherry-picked onto
  `feat/provisioning-protocomm` as the foundation, then extended.
- `feat/softap-provisioning` is kept until this branch is
  hardware-validated and merged, then deleted.
- Firmware drops the Improv source files entirely; no recovery path
  on UART0.
- A new DNS server task runs only during the provisioning window
  (~150 lines of UDP socket code, no extra component dependency).
- Captive-portal HTTP endpoints (`/generate_204`, `/hotspot-detect.html`,
  `/ncsi.txt`, `/connecttest.txt`, plus a wildcard 302) are added to
  the existing `esp_http_server` URI handler set.
- E-ink display gets a WiFi QR rendering path alongside the existing
  text rendering. QR string format documented in `BOARD_NOTES.md`.
- `flash-server` keeps only the ESP Web Tools flasher; the Improv
  config UI (`improv-protocol.js`, the dynamic 4-section form in
  `config-ui.js`) is deleted.
- AP password is a 12-character random alphanumeric string generated
  once at first boot via `esp_fill_random` and persisted in NVS under
  `nvs_namespace="devdash" key="ap_pwd"`. `storage_erase` (factory
  reset) clears the key, so the next boot regenerates. Earlier drafts
  used a MAC-derived password; switching to random-at-first-boot raises
  entropy from ~48 bits to ~71 bits and removes the cross-device
  collision risk for two boards sharing the same MAC suffix.
  Threat model: physical proximity is required to read the QR or the
  displayed credentials, the provisioning window is short and
  user-triggered, and the LAN configuration written through the portal
  is not high-value secret material in this hobby setup. If a future
  requirement changes the threat model, revisit by promoting to
  ADR-0001's path.

## Validation

End-to-end on the WeAct Super Mini:
- Flash fresh device → e-ink shows WiFi QR + portal URL.
- Scan with iOS Camera and Android Camera → AP joined, portal popped
  automatically.
- Save WiFi + API settings → device reboots, connects, fetches
  dashboard.
- Press BOOT during deep sleep → device wakes, re-enters portal mode.
- Edit API URL in portal → save → reboot → new API URL active.

## References

- [ADR-0001](0001-provisioning-transport.md) — the deferred
  protocomm/BLE/Sec2 direction
- `feat/softap-provisioning` commit `d8513a5` — the foundation cherry-pick
- WiFi QR format spec — Zxing/Java: `WIFI:T:<type>;S:<ssid>;P:<password>;;`
- Captive portal detection endpoints:
  - Android: `http://connectivitycheck.gstatic.com/generate_204`
  - iOS / macOS: `http://captive.apple.com/hotspot-detect.html`
  - Windows: `http://www.msftncsi.com/ncsi.txt`,
    `http://www.msftconnecttest.com/connecttest.txt`
