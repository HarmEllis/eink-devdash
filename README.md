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
| `CODEX_OVERAGE_USD` | no | empty | Manual overage spend in USD for the Codex extra-usage bar (ChatGPT-auth exposes no dollar figure). Empty or `0` hides the bar; a value > 0 shows a `$` symbol, an amount-capped bar, and the amount. |
| `CLAUDE_OVERAGE_USD` | no | empty | Optional override for the Claude extra-usage bar, which is otherwise read live from Claude's OAuth usage credits (currency symbol, bar = % of the monthly cap, amount consumed). If set (> 0) it forces a manual USD amount-capped bar. |
| `DASHBOARD_LOCALE` | no | `nl-NL` | BCP-47 locale for the extra-usage amount's decimal separator (`nl-NL` → `0,91`, `en-US` → `0.91`). Invalid/unsupported values fall back to the default. |
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

Dashboard and OTA manifest payloads are fetched strictly on demand: the Worker
does not store dashboard snapshots, and each device request is forwarded to an
active API publisher over the WebSocket.

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

The setup command authenticates Wrangler when needed, creates a relay identity
for the current machine, deploys the Worker, adds UUID-scoped credentials,
updates the local root `.env`, and prints QR codes for the device's API URL and
token fields. Running setup from another machine adds another independent
identity to the same Worker and does not replace existing identities.

To show the QR codes again without rotating credentials:

```bash
cd relay
npm run qr
```

Re-running `npm run setup` when the local `.env` already holds a complete
identity **asks** whether to reuse it or generate a new one:

- **Reuse** (the default on a bare Enter) keeps this machine's current device
  working — setup just re-pushes the same UUID-scoped secret and redeploys.
- **New** mints a fresh identity for *this* `.env` only. Each identity lives in
  its own UUID-scoped Worker secret, so this never touches another machine's or
  device's credentials. But because the API publisher dials out under exactly
  one UUID, choosing "new" means this machine's **old device must be
  re-provisioned** with the new URL/token. The old Worker secret stays
  authorized but unused; delete it with
  `wrangler secret delete DEVICE_IDENTITY_<oldUUID>` once you no longer need it.

To add **another** device without touching any existing one, run setup against a
fresh, empty output (a new machine, or a clean `relay-out/` for the Docker
image): with no identity present it generates a new one and adds it alongside
the others.

Set `RELAY_SETUP_IDENTITY=reuse` or `RELAY_SETUP_IDENTITY=new` to answer
non-interactively (CI, or `docker run` without `-it`). `reuse` errors out if the
target `.env` has no complete identity, so a missing or mis-mounted file fails
loudly instead of silently creating an extra identity.

### Setup without cloning (Docker)

If you would rather not clone the repo or install Node, run the prebuilt setup
image. It deploys the Worker to *your* Cloudflare account, pushes the secrets,
and writes a `.env` to the mounted output directory.

Authenticate one of two ways:

**API token (recommended).** Create a token in the Cloudflare dashboard
(My Profile → API Tokens → "Edit Cloudflare Workers" template). Scope it to the
target account and give it an expiry. Read it into the environment with a silent
prompt so the value is not recorded in your shell history:

```bash
mkdir -p relay-out
read -rs CLOUDFLARE_API_TOKEN && export CLOUDFLARE_API_TOKEN   # paste, then Enter
export CLOUDFLARE_ACCOUNT_ID=...     # only if the token spans multiple accounts
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  -e CLOUDFLARE_API_TOKEN -e CLOUDFLARE_ACCOUNT_ID \
  -v "$PWD/relay-out":/out \
  ghcr.io/harmellis/eink-devdash-relay-setup:latest
```

(Or put the token in a `chmod 600` file and pass `--env-file`.)

**Browser login (no token).** Map the OAuth callback port and copy the printed
URL into your browser; the `localhost:8976` redirect is forwarded into the
container:

```bash
mkdir -p relay-out
docker run --rm -it -p 8976:8976 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD/relay-out":/out \
  ghcr.io/harmellis/eink-devdash-relay-setup:latest
```

Notes:

- Use a dedicated output dir (`relay-out/`), not a directory that already holds
  an unrelated `.env`. Setup merges the current machine's relay values into it.
- If your Cloudflare account has no `workers.dev` subdomain yet, register one
  once in the dashboard (Workers & Pages → set up a subdomain) before running;
  `wrangler deploy` cannot create it from this non-interactive container.
- `-it` is needed for the browser-login flow, to render the provisioning QR
  codes, and for the reuse/new identity prompt when the mounted `.env` already
  holds an identity. Without `-it`, set `RELAY_SETUP_IDENTITY=reuse|new` to
  choose non-interactively (the default is reuse for a complete `.env`). It does
  not answer the deploy step (its stdin is ignored).
- Add `-e RELAY_SETUP_PRINT_ENV=1` to also echo the `.env` block (which includes
  `RELAY_PUBLISH_KEY`) to the terminal — handy when you cannot mount a volume.
  The device token and admin key are printed in the summary regardless.

The image produces `relay-out/.env`. To run the publisher without cloning the
repo, fetch the maintained Compose file and point it at that env file (this
keeps the host UID/GID mapping, Codex/Claude mounts, and restart policy in sync
rather than hand-rolling a long `docker run`):

To show the provisioning QR codes again without rotating credentials:

```bash
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  -v "$PWD/relay-out":/out \
  --entrypoint npm \
  ghcr.io/harmellis/eink-devdash-relay-setup:latest run qr
```

```bash
curl -O https://raw.githubusercontent.com/HarmEllis/eink-devdash/main/docker-compose.yml
cp relay-out/.env .env
printf 'HOST_UID=%s\nHOST_GID=%s\n' "$(id -u)" "$(id -g)" >> .env  # match your user
docker compose up -d
```

If you cloned the repo, just copy `relay-out/.env` to the root `.env` and run
`docker compose up -d`.

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

Check relay status with the per-device `RELAY_ADMIN_KEY` written during setup:

```bash
curl -H "Authorization: Bearer <RELAY_ADMIN_KEY>" \
  "https://<worker>.workers.dev/admin/stats?uuid=<DEVICE_UUID>"
```

The API sends a WebSocket heartbeat every 30 seconds and reconnects when the
relay does not answer within 10 seconds. These defaults can be overridden with
`RELAY_HEARTBEAT_INTERVAL_MS` and `RELAY_HEARTBEAT_TIMEOUT_MS`. The heartbeat
also verifies the relay protocol version; an outdated deployment remains
connected for backward compatibility but produces an API log warning asking
you to update the relay.

Cloudflare caching must remain disabled for `/d/*`, especially OTA manifests.
Verify the deployed behavior with:

```bash
curl -H "Authorization: Bearer <RELAY_ADMIN_KEY>" \
  "https://<worker>.workers.dev/admin/cache-bypass-probe?uuid=<DEVICE_UUID>"
```

The relay encrypts traffic in transit, but it is not end-to-end encrypted:
Cloudflare terminates TLS and can access the dashboard payload. The generated
`.env` stores bearer credentials in plaintext and should remain private.
After suspected exposure, replace the local identity and delete its old
`DEVICE_IDENTITY_<UUID_WITHOUT_HYPHENS>` Worker secret with Wrangler.

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

### Resetting the device

While the setup portal is open, a second long BOOT press opens a
non-destructive reset confirm screen. Nothing is erased until you act:

| BOOT gesture | Result |
|--------------|--------|
| Tap (short press) | **Config reset** — clears all saved WiFi networks, API endpoints, and quiet hours; keeps the display variant, refresh interval, and max-partial settings |
| Hold for about 3 seconds | **Full erase** — wipes the entire NVS; the device reboots as if first-run |
| No press | Cancel and return to the setup portal |

A tap commits the config reset immediately; the full erase requires a sustained
hold, so the destructive action cannot be triggered by accident. After either,
the device shows a confirmation and reboots. If a config reset cannot be saved
(for example a full NVS), a **RESET FAIL** screen offers a retry (one press) or
returns to the portal on timeout.

## Architecture

```text
Docker host
  API server (Fastify / TypeScript)
    GitHub counters and notifications
    Claude Code usage
    Codex usage
    Optional outbound relay publisher
          |
          | HTTP on LAN or on-demand HTTPS through the relay
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
