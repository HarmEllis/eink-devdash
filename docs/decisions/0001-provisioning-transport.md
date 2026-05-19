# ADR-0001: Provisioning transport — BLE + SoftAP, not Web Serial

- **Status:** Accepted
- **Date:** 2026-05-19
- **Branch where adopted:** `feat/provisioning-protocomm`
- **Supersedes:** the Improv-over-serial path on `main` (commits `9b4c201`, `1be103d`, `56969a0`) and the experimental HTTP SoftAP portal on `feat/softap-provisioning` (commit `d8513a5`).

## Context

End users need to configure Wi-Fi credentials and API endpoints on the
ESP32-S3 dashboard without flashing custom firmware and without
soldering a second cable. The original design used Improv WiFi Serial
over the browser Web Serial API, served from the flash-server UI on
the same page that flashes the device.

This worked on bench testing with an ESP32-S3-DevKitC-1, but failed
reproducibly on the production target — the WeAct ESP32-S3 Super Mini
— with `rst:0x15 (USB_UART_CHIP_RESET)` immediately after the browser
called `port.open()`. See the night-of-2026-05-18 saga for the full
debugging path.

## Decision

Provisioning runs on **ESP-IDF Unified Provisioning** (`wifi_prov_mgr`
+ `protocomm`) with **both BLE GATT and SoftAP+HTTP transports active
simultaneously** during the provisioning window, using **Security 2
(SRP6a)** for the session handshake. Custom protocomm endpoints carry
the multi-network and multi-API configuration on top of the
stock `prov-config` endpoint.

The browser side uses a protocomm-compatible client (either
`esp-idf-provisioning-web` or a thin re-implementation) — **not**
Web Serial.

## Why Web Serial does not work here (now)

The blocker is a hardware behaviour of the ESP32-S3, not a protocol
problem. Switching from Improv to protocomm-over-serial would not
change anything because the failure happens below the protocol layer.

### Two USB paths on the ESP32-S3

| Path | Hardware | Reset behaviour on `port.open()` |
|---|---|---|
| Native USB → built-in **USB-Serial-JTAG peripheral** (GPIO19/20) | On-chip CDC endpoint | Host-controlled chip-reset whenever the host asserts CDC control-line state. Chrome's Web Serial does this on every `port.open()` — see Espressif issue [#8889](https://github.com/espressif/esp-idf/issues/8889). |
| External **USB-UART bridge chip** (CP2102N, CH340, FT232) on UART0 (GPIO43/44) | Off-chip USB-to-serial IC with DTR/RTS reset circuit | Reset only fires when DTR/RTS are toggled in the specific flash-mode entry sequence. Regular `port.open()` is safe. |

### Why the Super Mini fails where the DevKitC works

- **ESP32-S3-DevKitC-1** exposes both ports. The "UART" port routes
  through a CP2102N to UART0 → safe for Web Serial. The "USB" port
  routes through the USB-Serial-JTAG peripheral → unsafe. Most users
  plug into UART by default and never see the issue.
- **WeAct ESP32-S3 Super Mini** has only the native USB path. No
  external bridge chip. There is no Web Serial route that does not
  hit the USB-Serial-JTAG reset.

The previously-claimed working Improv path on this repo (`9b4c201`)
opened `/dev/uart/0`, which on the Super Mini is exposed only on bare
TX/RX pins and requires an external USB-UART debugger. That is not a
distributable end-user flow.

## Alternatives considered

### A. Improv over Web Serial — rejected

The original path. Fails on the Super Mini for the hardware reason
above. Not portable across S3 boards.

### B. Protocomm over Web Serial — rejected

Suggested by an external research note that conflated `esptool-js`
(flash-mode protocol, runs only after chip-reset) with `protocomm`
(runtime command-response over a transport). The Espressif
documentation for both `wifi_prov_mgr` and `protocomm` lists only
BLE, SoftAP+HTTPD, and an internal Console transport. The official
client SDK [`esp-idf-provisioning-android`](https://github.com/espressif/esp-idf-provisioning-android)
supports only BLE and SoftAP. There is no Espressif-supplied
serial protocomm transport, and a DIY one would still hit the same
chip-reset on the Super Mini.

### C. TinyUSB CDC instead of USB-Serial-JTAG — deferred

It is technically possible to disable the built-in USB-Serial-JTAG
peripheral and implement a CDC endpoint via TinyUSB with custom
line-state handling. This would unblock Web Serial provisioning on
the Super Mini.

Reasons we are not doing this now:
- It is a substantial new sub-project (USB stack, descriptors,
  line-state filtering, regression testing) for a single transport.
- It costs the built-in JTAG debug capability on that port.
- BLE and SoftAP already give us a universally-portable provisioning
  path with no board-specific assumptions.

Revisit if a future requirement makes single-cable Web Serial
provisioning load-bearing.

### D. Require a DevKitC-class board — rejected

The Super Mini was chosen for form factor and cost. Excluding it
defeats the product premise.

## Consequences

- Firmware uses `wifi_prov_mgr` with both `wifi_prov_scheme_softap`
  and `wifi_prov_scheme_ble` (or `protocomm` directly with both
  transports attached, if `wifi_prov_mgr` cannot host both at once —
  to be confirmed in the implementation plan).
- Security 2 (SRP6a) is mandatory; salt/verifier provisioned at
  build time or first boot.
- Custom protocomm endpoints replace the Improv RPCs from `9b4c201`
  for multi-network and multi-API configuration.
- The Improv code path in `firmware/main/improv.c` is removed (or
  gated off by Kconfig and built out by default).
- The flash-server `improv-protocol.js` UI is replaced by a
  protocomm client. The flash-server keeps its job of flashing the
  binary via ESP Web Tools; provisioning moves out of that page.
- The branch `feat/softap-provisioning` is dropped in favour of
  this branch.

## References

- ESP-IDF Unified Provisioning: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/provisioning/provisioning.html
- ESP-IDF protocomm: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/provisioning/protocomm.html
- Espressif issue #8889 (USB-Serial-JTAG host-reset on CDC open)
- `esp-idf-provisioning-android` (official client SDK, BLE + SoftAP only)
- `ho-229/esp-idf-provisioning-web` (third-party browser SDK, BLE + SoftAP)
