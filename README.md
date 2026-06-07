# eink-devdash

A physical developer dashboard for an ESP32-S3 and a 2.9-inch e-ink display.
It shows GitHub activity, Claude Code limits, and Codex usage, then deep-sleeps
between configurable refreshes.

The same firmware supports the WeAct Studio 2.9-inch SSD1680 Black/White (BW)
and Black/White/Red (BWR) panels. BW is recommended for faster partial
refreshes; BWR adds red alert highlights.

[![CI](https://github.com/HarmEllis/eink-devdash/actions/workflows/ci.yml/badge.svg)](https://github.com/HarmEllis/eink-devdash/actions/workflows/ci.yml)
[![Docker image](https://img.shields.io/badge/ghcr.io-eink--devdash-blue?logo=docker)](https://github.com/HarmEllis/eink-devdash/pkgs/container/eink-devdash)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

| Boot screen | Dashboard | OTA update |
|-------------|-----------|------------|
| <img src="docs/assets/readme-boot-screen.svg" alt="DevDash boot screen" width="280"> | <img src="docs/assets/readme-dashboard-screen.svg" alt="DevDash dashboard screen" width="280"> | <img src="docs/assets/readme-ota-screen.svg" alt="DevDash OTA update screen" width="280"> |

Red ink highlights alerts such as Dependabot findings, high usage, and
authentication errors.

## Contents

- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Optional Cloudflare relay](#optional-cloudflare-relay)
- [Flashing the firmware](#flashing-the-firmware)
- [Device setup and operation](#device-setup-and-operation)
- [Architecture](#architecture)
- [API reference](#api-reference)
- [Development](#development)
- [License](#license)

## Hardware

| Part | Specification |
|------|---------------|
| MCU | ESP32-S3 Super Mini |
| Display | WeAct Studio 2.9-inch SSD1680 e-ink, 128x296 pixels, BW or BWR |
| Interface | SPI |

Both panel variants use the same wiring and firmware image. Select the panel
type in the device setup portal after flashing.

| Panel | Advantages | Tradeoffs |
|-------|------------|-----------|
| BW | Fast per-region partial refreshes and fewer full-screen flashes | Alerts render in black |
| BWR | Red alert highlights | Every dashboard update requires a slower full-color refresh |

### Wiring

| Display pin | ESP32-S3 GPIO | Typical wire color |
|-------------|---------------|--------------------|
| SDA (MOSI) | 11 | Yellow |
| SCL (SCK) | 12 | Green |
| CS | 10 | Blue |
| D/C | 9 | White |
| RES | 1 | Orange |
| BUSY | 13 | Purple |
| VCC | 3V3 | Red |
| GND | GND | Black |

## Quick start

The normal installation uses the published Docker image and hosted web
flasher. No local firmware toolchain is required.

### 1. Start the API

Clone the repository, create the environment file, and configure at least
`DEVICE_TOKEN`, `HOST_UID`, and `HOST_GID`:

```bash
cp .env.example .env
id -u
id -g
openssl rand -hex 32
```

Use the command output to update `.env`, then start the API:

```bash
docker compose pull
docker compose up -d
```

The API listens on `http://<host>:3000`. Verify it with:

```bash
curl http://<host>:3000/health
# {"ok":true}
```

Keep port 3000 on a trusted local network. Direct LAN connections use HTTP, so
the bearer token is not confidential on an untrusted network. Use the optional
Cloudflare relay when the device needs an internet-reachable HTTPS endpoint.

For more reliable `.local` discovery on Linux Docker hosts, run the host-network
service instead of the default service:

```bash
docker compose down
docker compose --profile mdns-host pull
docker compose --profile mdns-host up -d api-mdns-host
```

The API then advertises `http://devdash-api.local:3000` over mDNS. Run only one
of the two API services.

To pin the container to a release, set `IMAGE_TAG`, for example:

```env
IMAGE_TAG=v0.3.1
```

### 2. Flash the device

Open <https://harmellis.github.io/eink-devdash/> in desktop Chrome or Edge,
connect the ESP32-S3 over USB, and click **Install**.

### 3. Configure the device

After flashing, scan the WiFi QR code shown on the display. In the captive
portal, enter:

- your WiFi credentials;
- the API URL, such as `http://192.168.1.50:3000`;
- the same `DEVICE_TOKEN` used by the API;
- your panel type and preferred refresh settings.

The device reboots, fetches the dashboard, and starts its sleep/refresh cycle.

## Configuration

Set API configuration in the root `.env` file. The included `.env.example`
contains all supported variables.

### Main settings

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `DEVICE_TOKEN` | yes | none | Shared bearer token used by the device. Use at least 32 random characters. |
| `HOST_UID` | recommended | `1000` | UID of the host user that owns `~/.claude`. |
| `HOST_GID` | recommended | `1000` | GID of the host user that owns `~/.claude`. |
| `GITHUB_TOKEN` | no | empty | Token for issues, pull requests, and Dependabot alerts. |
| `GITHUB_NOTIFICATIONS_TOKEN` | no | empty | Classic token with `notifications` scope for unread notifications. |
| `CODEX_PLAN_TYPE` | no | empty | Set to `plus` or `team` when multiple ChatGPT accounts are available. |
| `CODEX_LIVE_USAGE` | no | `true` | Set to `false` to use only the on-disk Codex session fallback. |
| `DASHBOARD_TIME_ZONE` | no | `Europe/Amsterdam` | IANA timezone used for timestamps and quiet hours. |
| `MDNS_ENABLED` | no | `true` | Enables `.local` advertisement. |
| `MDNS_NAME` | no | `devdash-api` | Hostname advertised under `.local`. |
| `OTA_ENABLED` | no | `true` | Set to `false` to stop advertising firmware updates. |
| `IMAGE_TAG` | no | `latest` | Docker image tag to run. |

`GITHUB_TOKEN` may be a fine-grained token with read access to Dependabot
alerts. Classic tokens need `security_events`; add `repo` only when private
repository issues and pull requests must be counted. GitHub's notifications
API requires a separate classic token with `notifications` scope.

The container mounts `~/.claude` read-write so it can refresh Claude OAuth
credentials, and mounts `~/.codex` read-only for Codex authentication and
session data. Matching `HOST_UID` and `HOST_GID` prevents permission failures
when the API refreshes the Claude credentials file.

## Optional Cloudflare relay

The self-hosted relay lets the device reach the API from outside the LAN
without exposing port 3000. It deploys a Cloudflare Worker in your own account
and transports dashboard requests between the device and the API over HTTPS
and an outbound WebSocket.

```text
ESP32 -- HTTPS --> Cloudflare Worker <-- outbound WSS -- API container
```

### Setup

With Node.js installed:

```bash
cd relay
npm install
npm run setup
cd ..
docker compose up -d
```

The setup command authenticates Wrangler when needed, generates the relay
credentials, deploys the Worker, updates the root `.env`, and prints QR codes
for the device's API URL and token fields.

Re-running `npm run setup` rotates the generated identity and credentials.
Reconfigure the device afterwards.

### LAN and relay profiles

Enabling the relay does not disable the local `/dashboard` route. A device can
store both endpoints and try them in order:

| Route | API URL example | Transport |
|-------|-----------------|-----------|
| Direct LAN | `http://192.168.1.50:3000` | HTTP on the trusted LAN |
| Relay | `https://<worker>.workers.dev/d/<uuid>` | HTTPS through Cloudflare |

Use the same `DEVICE_TOKEN` for both profiles. Always include a token; profiles
with an empty token are skipped.

### Relay operations and security

Check relay status with the `ADMIN_KEY` printed during setup:

```bash
curl -H "Authorization: Bearer <ADMIN_KEY>" \
  "https://<worker>.workers.dev/admin/stats?uuid=<DEVICE_UUID>"
```

Cloudflare caching must remain disabled for `/d/*`, especially OTA manifests.
Verify the deployed behavior with:

```bash
curl -H "Authorization: Bearer <ADMIN_KEY>" \
  "https://<worker>.workers.dev/admin/cache-bypass-probe?uuid=<DEVICE_UUID>"
```

The relay encrypts traffic in transit, but it is not end-to-end encrypted:
Cloudflare terminates TLS and can access the dashboard payload. The generated
`.env` stores bearer credentials in plaintext and should remain private.
Rotate `DEVICE_TOKEN` and `RELAY_PUBLISH_KEY` after suspected exposure.

The firmware accepts only canonical GitHub release URLs for relay-provided OTA
updates and rejects downgrades. It does not independently verify a firmware
release signature.

## Flashing the firmware

### Hosted web flasher

The recommended flasher publishes the current release and works through the
Web Serial API:

1. Connect the ESP32-S3 over USB.
2. Open <https://harmellis.github.io/eink-devdash/> in desktop Chrome or Edge.
3. Click **Install** and select the serial port.
4. Erase the device when prompted for a fresh installation.

Web Serial is not supported by Firefox, Safari, or mobile browsers.

### Command-line flash

Release assets include the required binaries and `SHA256SUMS`. This path
requires `gh`, `sha256sum`, `esptool.py`, and direct USB access:

```bash
TAG=v0.3.1
mkdir -p bins
cd bins
gh release download "$TAG" --repo HarmEllis/eink-devdash \
  --pattern '*.bin' --pattern 'SHA256SUMS'
sha256sum -c SHA256SUMS
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0xf000  ota_data_initial.bin \
  0x20000 eink-devdash.bin
```

### Updates

After initial setup, the device checks the API's OTA manifest on wake and
installs newer published firmware automatically when OTA is enabled.

Devices still using the original v0.1.x single-app partition layout must be
flashed once with the hosted web flasher and erased. This installs the OTA
partition layout and clears saved WiFi and API settings.

## Device setup and operation

On first boot, and after a long BOOT-button press, the display shows a WiFi QR
code, access-point credentials, and `http://192.168.4.1`.

1. Scan the QR code and join the device's WiFi network.
2. Wait for the captive portal or open `http://192.168.4.1`.
3. Configure up to five WiFi networks and five API endpoints per network.
4. Save the settings and wait for the device to reboot.

API URLs may use `http://` or `https://`. HTTPS certificates are validated
against the ESP-IDF root certificate bundle.

| Setting | Notes |
|---------|-------|
| API URL | For example `http://192.168.1.50:3000`, `http://devdash-api.local:3000`, or a relay URL |
| Device token | Must match the API's `DEVICE_TOKEN` |
| Refresh interval | 3 to 60 minutes; default 5 |
| Quiet hours | Optional start/end time per WiFi network |
| Display | Select BW or BWR |
| Max partial refreshes | BW only; default 5 |

Empty password or token fields preserve the saved secret. Use the corresponding
**Clear** checkbox to remove one.

### Quiet hours

During a configured quiet-hours window, the device skips network and display
updates and wakes roughly hourly to check whether the window has ended. A short
BOOT press forces an immediate refresh.

The dashboard API supplies local time according to `DASHBOARD_TIME_ZONE`.
After power loss, the device performs one normal refresh before quiet hours
take effect again.

### BOOT button

| Action | Result |
|--------|--------|
| Short press during deep sleep | Wake and refresh immediately |
| Long press for about 5 seconds | Open the setup portal |

Do not hold BOOT while applying USB power: the ESP32-S3 enters ROM download
mode before the firmware starts. Power it first, then long-press BOOT.

If no saved WiFi network or API endpoint is reachable, the display shows the
failed connection type and the device sleeps before trying again. Saved
credentials are not erased automatically.

## Architecture

```text
Docker host
  API server (Fastify / TypeScript)
    GitHub counters and notifications
    Claude Code usage
    Codex usage
    Optional outbound relay publisher
          |
          | HTTP on LAN or HTTPS through the relay
          v
ESP32-S3 firmware
  WiFi provisioning
  Dashboard and OTA fetch
  E-ink rendering
  Deep sleep
```

The API exposes one provider-neutral dashboard schema. Provider adapters live
in `api/src/services/usage.adapters.ts` and
`api/src/services/code-host.adapters.ts`; the relay forwards the resulting
payload without provider-specific parsing.

## API reference

All protected routes require:

```text
Authorization: Bearer <DEVICE_TOKEN>
```

### `GET /health`

Returns `{ "ok": true }` without authentication.

### `GET /dashboard`

Returns a schema version 2 dashboard document with a bounded `services` array:

```json
{
  "schemaVersion": 2,
  "services": [
    {
      "id": "github",
      "kind": "code-host",
      "provider": "github",
      "label": "GitHub",
      "status": "ok",
      "counters": [
        { "id": "issues", "label": "Issues", "value": 3 },
        { "id": "pullRequests", "label": "Pulls", "value": 1 }
      ]
    },
    {
      "id": "codex",
      "kind": "usage",
      "provider": "codex",
      "label": "Codex",
      "status": "ok",
      "windows": [
        { "id": "short", "label": "5h", "usedPercent": 37 }
      ]
    }
  ],
  "updatedAt": "2026-05-16T14:32:00Z",
  "updatedAtLocal": "16:32",
  "updatedAtLocalIso": "2026-05-16T16:32:00"
}
```

Services are omitted when their provider or credentials are not configured.
Codex usage uses the live app-server response when available and falls back to
the latest local session data.

### `GET /ota/manifest`

Returns the current firmware release when OTA is enabled:

```json
{
  "otaEnabled": true,
  "latestVersion": "v0.3.1",
  "downloadUrl": "https://github.com/HarmEllis/eink-devdash/releases/download/v0.3.1/eink-devdash.bin"
}
```

Otherwise it returns:

```json
{ "otaEnabled": false }
```

## Development

### Devcontainer

Firmware and API development use the VS Code devcontainer in
[`.devcontainer/`](.devcontainer/). It provides ESP-IDF v5.3, Node.js, and the
project toolchain.

Open the repository in VS Code and choose **Reopen in Container**. Run firmware
commands inside that container.

### Repository layout

```text
api/            Fastify API server
firmware/       ESP-IDF firmware and display driver
flash-server/   Local and hosted ESP Web Tools flasher
relay/          Cloudflare Worker and Durable Object relay
scripts/        Project utilities
docs/decisions/ Architecture decision records
```

### API

Inside the devcontainer:

```bash
cd api
npm install
npm run build
npm test
npm run dev
```

To build and run the local container image:

```bash
docker compose build api
docker compose up -d api
```

### Firmware

Inside the devcontainer:

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
cd ../flash-server
bash serve.sh
```

Open `http://localhost:8080` in desktop Chrome or Edge. The host browser owns
the USB connection while the devcontainer builds and serves the binaries.

Use `flash-server/watch.sh` instead of `serve.sh` to republish binaries after
each successful firmware build.

### Relay

```bash
cd relay
npm install
npm run build
npm test
npm run dev
```

### Documentation

- Architecture decisions: [`docs/decisions/`](docs/decisions/)
- Firmware board and display notes:
  [`firmware/BOARD_NOTES.md`](firmware/BOARD_NOTES.md)
- Firmware test instructions: [`firmware/test/README.md`](firmware/test/README.md)
- Release history: [`CHANGELOG.md`](CHANGELOG.md)

Regenerate the README screen previews after display layout changes:

```bash
node scripts/render-readme-screens.mjs
```

## License

MIT. See [LICENSE](LICENSE).
