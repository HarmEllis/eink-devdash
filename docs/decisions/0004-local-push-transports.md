# ADR-0004: Local push transports for workplace networks

- **Status:** Proposed
- **Date:** 2026-05-22

## Context

The dashboard currently uses a WiFi pull model: the ESP32-S3 wakes, connects to
WiFi, fetches `GET /dashboard` from the Docker-hosted API, renders the response,
and goes back to sleep.

This is reliable on stable home networks, but it is awkward on workplace
networks where:

- the laptop receives a different IP address regularly;
- the API runs inside Docker on WSL;
- mDNS from the container is not reliable or not available;
- the ESP cannot count on a stable `http://host:3000` endpoint.

Laptop hotspot mode is not an acceptable workaround for this scenario.

## Idea

Add a local push path next to the existing WiFi API pull path. In local push
mode, the ESP does not start WiFi for dashboard refreshes. Instead, a browser
page served by the API fetches the dashboard JSON locally and pushes that JSON
to the ESP over a short-range local transport.

The firmware should keep dashboard parsing and rendering transport-independent:

```text
HTTP pull  \
BLE push    > framed dashboard JSON -> parse -> render -> sleep
USB push   /
```

The payload should use a small framed protocol rather than raw JSON:

```text
DDASH1
length:1234
crc32:abcd1234

{ ... dashboard json ... }
```

The same frame format can be reused by BLE, TinyUSB CDC, and any later local
transport.

## BLE push

BLE push is the preferred first implementation for this workplace use case.

Flow:

1. The ESP boots in `ble_push` mode, or in `auto` mode with a BLE receive window.
2. WiFi is not started while waiting for a pushed dashboard payload.
3. The ESP advertises a DevDash BLE service.
4. The API web page uses Web Bluetooth to connect to the ESP.
5. The page fetches `GET /dashboard` from the local API.
6. The page writes the framed payload in BLE chunks.
7. The ESP validates length, CRC, schema, and optional token.
8. On success, the ESP renders and enters deep sleep.

Benefits:

- avoids the unstable workplace IP/mDNS problem;
- avoids the ESP32-S3 USB-Serial-JTAG browser reset issue;
- works without the ESP reaching the Docker container over LAN;
- keeps the current API response schema.

Tradeoffs:

- Web Bluetooth support is browser and platform dependent;
- BLE chunking, MTU handling, reconnects, and progress acknowledgements add
  firmware and browser complexity;
- pairing or a local push token should be used before accepting dashboard data.

## Web Serial push via TinyUSB CDC

Plain Web Serial over the current native USB-Serial-JTAG port is not a reliable
runtime transport on the WeAct ESP32-S3 Super Mini. ADR-0001 documents the
reason: Chrome opening the USB-Serial-JTAG CDC port can trigger
`USB_UART_CHIP_RESET` before any application protocol runs. Improv was not the
root cause; the reset happens below the protocol layer.

Web Serial should only be revisited as a product-quality push path if the
firmware exposes its own TinyUSB CDC ACM interface and handles USB line-state
changes without resetting the chip.

Flow:

1. The ESP boots in `usb_push` mode, or in `auto` mode with a USB receive window.
2. WiFi is not started while waiting for a pushed dashboard payload.
3. The browser connects to the custom TinyUSB CDC interface via Web Serial.
4. The API web page fetches `GET /dashboard` from the local API.
5. The page writes the same framed dashboard payload used by BLE.
6. The ESP validates the frame, renders, and enters deep sleep.

Benefits:

- no LAN reachability between ESP and Docker is required;
- larger payloads are easier than BLE;
- the browser page can reuse most of the BLE push UI and framing code.

Tradeoffs:

- TinyUSB CDC is a substantial firmware sub-project;
- USB descriptors, enumeration, wake-from-deep-sleep behavior, and logging/debug
  workflows need careful testing;
- it may constrain or replace the current USB-Serial-JTAG logging/debugging
  setup;
- plain Web Serial over USB-Serial-JTAG remains rejected for the Super Mini.

## Firmware configuration

Add a dashboard transport setting in NVS:

```text
transport = wifi_pull | ble_push | usb_push | auto
```

Expected behavior:

- `wifi_pull`: current behavior; connect to WiFi and fetch the API.
- `ble_push`: keep WiFi off; advertise BLE and wait for a pushed payload.
- `usb_push`: keep WiFi off; wait for a TinyUSB CDC framed payload.
- `auto`: wait briefly for local push, then fall back to WiFi pull if no valid
  payload arrives.

The provisioning portal should eventually expose this setting. Existing WiFi and
API profiles remain useful for `wifi_pull` and `auto`.

## API web page

Add a browser page served by the API, for example `GET /push`.

The page should:

- fetch the current dashboard JSON from `GET /dashboard`;
- connect to the ESP over Web Bluetooth;
- later connect over Web Serial when TinyUSB CDC exists;
- send the framed payload;
- show connection, transfer, validation, and render status;
- optionally support "push now" and an auto-push interval while the page remains
  open.

## Open questions

- Should local push use the existing device token or a separate local push token?
- Should the ESP store the last valid pushed JSON for display after timeout?
- What is the receive window for push-only and auto modes?
- Should BLE push be available only on explicit boot-button entry, or also as a
  persistent configured transport?
- Can TinyUSB CDC coexist cleanly with the current USB-Serial-JTAG console setup
  on the Super Mini?

## Recommendation

Implement BLE push first. Keep the frame format and dashboard ingest path
transport-independent so TinyUSB CDC Web Serial can be added later without
changing dashboard parsing or rendering.
