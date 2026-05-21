# eink-devdash

A physical developer dashboard on a 2.9" black/red e-ink display, driven by an ESP32-S3. Shows GitHub activity, Claude Code rate limits, and Codex usage — updated on a configurable interval via deep sleep.

```
┌─────────────────────────────────┐
│  GitHub          Claude Code    │
│  Issues: 3       5h: 42/50      │
│  PRs:    1       Week: 210/1000 │
│  Alerts: 0  ███  Codex 5h: 37%  │
│  Updated: 14:32                 │
└─────────────────────────────────┘
```

Red ink highlights alerts: Dependabot findings, usage above 80%, or auth errors.

---

## Hardware

| Part | Spec |
|------|------|
| MCU | ESP32-S3 Super Mini |
| Display | WeAct 2.9" Black/Red e-ink (128×296 px) |
| Driver IC | SSD1680 (SPI) |

### Wiring

| Display pin | ESP32-S3 GPIO | Color |
|-------------|---------------|-------|
| SDA (MOSI) | 11 | Yellow |
| SCL (SCK) | 12 | Green |
| CS | 10 | Blue |
| D/C | 9 | White |
| RES | 1 | Orange |
| BUSY | 13 | Purple |
| VCC | 3V3 | Red |
| GND | GND | Black |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Proxmox VM (Docker)                                        │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  API server  (Fastify / TypeScript, port 3000)       │   │
│  │  ├── GitHub service  (PAT → issues, PRs, Dependabot) │   │
│  │  ├── Claude service  (OAuth token → rate limits)     │   │
│  │  └── Codex service   (ChatGPT-auth Codex sessions)   │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
          ▲  Bearer token over HTTP (LAN only)
          │
┌─────────────────────────────────────────────────────────────┐
│  ESP32-S3                                                   │
│  ├── NVS: stores API URL, token, refresh interval          │
│  ├── WiFi provisioning (SoftAP + QR on first boot)         │
│  ├── HTTP fetch → parse JSON → render to e-ink             │
│  └── Deep sleep between refreshes                          │
└─────────────────────────────────────────────────────────────┘
```

### Refresh strategy

| Mode | Duration | Trigger |
|------|----------|---------|
| BW fast monochrome | ~2-4 s | Dashboard metrics changed while both the previous and current frame are black/white-only |
| Full 3-color (Mode 1 LUT) | ~15-27 s | Red content changed, previous frame had red, first render, controller wake, or after ten fast monochrome refreshes |

Timestamp and reset-countdown changes by themselves do not repaint the panel.

Minimum refresh interval: 3 minutes (configurable 3–60 min).

---

## Getting started

### Prerequisites

- Docker + Docker Compose (for the API server)
- [ESP-IDF v5.3](https://docs.espressif.com/projects/esp-idf/en/v5.3/) or the included dev container
- A GitHub Personal Access Token with `repo` and `security_events` scopes
- Codex CLI authenticated with ChatGPT in the mounted `~/.codex` directory

### 1. API server

Create a `.env` file in the repo root:

```env
GITHUB_TOKEN=ghp_...
CODEX_PLAN_TYPE=
CODEX_LIVE_USAGE=true
CODEX_CLI_PATH=
CODEX_APP_SERVER_TIMEOUT_MS=8000
DEVICE_TOKEN=<random 32-char secret you generate>
MDNS_ENABLED=true
MDNS_NAME=devdash-api
```

Start the server:

```bash
docker compose up -d
```

The API is available at `http://<your-vm-ip>:3000`. It also advertises
`http://devdash-api.local:3000` over mDNS by default when the container network
allows multicast. The `/health` endpoint requires no authentication; all other
routes require `Authorization: Bearer <DEVICE_TOKEN>`.

For reliable `.local` discovery on Linux Docker hosts, use host networking:

```bash
docker compose --profile mdns-host up -d api-mdns-host
```

Bridge networking with the default `api` service still works for direct IP URLs,
but mDNS advertisement can be inconsistent across Docker hosts and VLANs.

### 2. Firmware

Open the project in VS Code with the dev container, or install ESP-IDF v5.3 manually.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

On first boot the display shows the V4 provisioning screen with a QR code, the
SoftAP name, the AP password, and `192.168.4.1`. Both first-time setup and
later edits use the same on-device captive portal:

1. **Scan the QR with your phone camera.** iOS Camera and Android Camera /
   Google Lens both recognise the `WIFI:T:WPA;S:devdash-XXXX;P:…;;` payload
   and offer one-tap join.
2. **The captive-portal sheet pops up automatically** (iOS/Android/Windows).
   If it does not, open `http://192.168.4.1` in any browser while still
   joined to the AP.
3. **Edit the form** — up to five WiFi networks × five API endpoints each.
   Empty password / token fields are treated as "keep the saved value";
   tick the matching *Clear* checkbox to erase a stored secret.
4. **Save.** The device shows a confirmation, reboots ~4 seconds later, joins
   WiFi, fetches the API, and renders the dashboard.

The AP password is a 12-character random alphanumeric string generated once
at first boot and persisted in NVS. The same password keeps working across
reboots; a factory reset (`idf.py erase-flash`) regenerates it. API URLs
must use `http://`; IP addresses, DNS names, and `.local` mDNS names are
accepted. The HTTPS path is out of scope for this firmware revision.

- API URL: `http://192.168.1.50:3000`
- Device token: required for each enabled API entry
- Refresh interval: 3–60 minutes, default 5

The device token must match `DEVICE_TOKEN` in the API server's `.env`.

If none of the stored WiFi networks are reachable, the device displays
`OFFLINE` and returns to deep sleep (or keeps retrying with
`CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE=y`). Stored credentials are
never erased automatically.

**Re-enter the captive portal at any time** by holding the BOOT button
(GPIO0) for ~5 seconds. The same threshold applies whether the device
is in deep sleep, fetching, rendering, or stuck in the offline retry
loop — `CONFIG_DEVDASH_BOOT_LONGPRESS_MS` (default 5000 ms) is the
single source of truth. A short press of BOOT while in deep sleep
wakes the device for an immediate refresh, without entering the
portal. The portal itself does not auto-close while open; it stays
available until the user saves credentials or power-cycles the device.

Known limitation: holding BOOT while applying USB power (cold boot)
puts the ESP32-S3 into ROM download mode at the hardware level, before
the firmware runs. Power on first, then long-press BOOT.

To wipe stored credentials and force a fresh portal session over USB:

```bash
parttool.py -p /dev/ttyACM0 erase_partition --partition-name=nvs
# or, full erase:
idf.py erase-flash
```

### 3. Web flash (optional)

Pre-built binaries can be flashed directly from a browser via the included
web flash server. Use `watch.sh` (recommended) — it starts the HTTP server
and automatically updates the served binaries whenever a new build completes:

```bash
cd flash-server
bash watch.sh
```

Open `http://localhost:8080` in Chrome and click Install. After flashing
completes the page redirects to `/flashed.html` (V4 S3 design) with a
step-by-step guide that mirrors the e-ink prompt and a troubleshooting
accordion. After each `idf.py build` the bins are refreshed automatically;
just click Install again to flash the new firmware. No need to restart the
server.

> `serve.sh` still works for a one-shot copy + serve, but requires a manual
> restart after every rebuild.

---

## API reference

### `GET /dashboard`

Returns current dashboard data. Requires `Authorization: Bearer <token>`.
The `github` object is omitted when `GITHUB_TOKEN` is unset or empty.
Set `CODEX_PLAN_TYPE=team` or `CODEX_PLAN_TYPE=plus` to pin Codex usage to a
specific ChatGPT plan when multiple accounts have local session history or
multiple live rate-limit buckets are available. Codex live usage is read
through `codex app-server` first; the API falls back to local session JSONL
files when the CLI, auth, or live endpoint is unavailable. Set
`CODEX_LIVE_USAGE=false` to disable the live probe, `CODEX_CLI_PATH` to use a
custom CLI binary, or `CODEX_APP_SERVER_TIMEOUT_MS` to tune the app-server
request timeout.

```json
{
  "schemaVersion": 1,
  "github": {
    "issues": 3,
    "prs": 1,
    "dependabot": 0,
    "authError": false
  },
  "claude": {
    "fiveHour": { "used": 42, "limit": 50, "resetInSeconds": 1823 },
    "weekly":   { "used": 210, "limit": 1000, "resetInSeconds": 302400 },
    "authError": false
  },
  "codex": {
    "source": "chatgpt",
    "planType": "plus",
    "short": {
      "usedPercent": 37,
      "label": "5h",
      "resetsAt": 1779232450,
      "resetInSeconds": 1823
    },
    "long": {
      "usedPercent": 27,
      "label": "7d",
      "resetsAt": 1779641619,
      "resetInSeconds": 302400
    },
    "reachedLimit": null
  },
  "updatedAt": "2026-05-16T14:32:00Z",
  "updatedAtLocal": "16:32"
}
```

### `GET /health`

Returns `{ "ok": true }`. No authentication required. Use for container health checks.

---

## Project structure

```
eink-devdash/
├── api/                    # Fastify API server (TypeScript)
│   ├── src/
│   │   ├── index.ts        # Server entry point + auth hook
│   │   └── routes/
│   │       └── dashboard.ts
│   ├── Dockerfile
│   └── package.json
├── firmware/               # ESP-IDF firmware (C)
│   ├── main/
│   │   ├── main.c          # App entry: provision → fetch → render → sleep
│   │   ├── api_client.h/c  # HTTP fetch + JSON parse
│   │   ├── display.h/c     # Layout renderer
│   │   ├── wifi_prov.h/c   # SoftAP provisioning
│   │   └── storage.h/c     # NVS read/write
│   └── components/
│       └── eink_weact29/   # SSD1680 driver component
├── flash-server/           # Browser-based OTA flash page
├── docker-compose.yml
└── .devcontainer/          # VS Code dev container (ESP-IDF)
```

---

## NVS layout

Namespace `devdash`.

| Key | Type | Description |
|-----|------|-------------|
| `api_url` | string | API server base URL (overrides Kconfig default) |
| `device_token` | string | Bearer token (overrides Kconfig default) |
| `refresh_min` | u8 | Refresh interval, 3–60 min (overrides Kconfig default) |

WiFi credentials, API URLs, bearer tokens, and refresh settings are stored in
the `cfg_v2` profile blob. The refresh-cycle bookkeeping
(`bw_fast_cycle_count`, `last_red_state`) lives in RTC slow memory and persists
across deep-sleep wakeups but resets on power-on.

---

## Data sources

| Source | How |
|--------|-----|
| GitHub | REST API v3 via PAT (`repo` + `security_events` scopes) |
| Claude Code | Reads `~/.claude/.credentials.json` (OAuth token) for rate-limit headers — no Anthropic API key required |
| Codex | Live `codex app-server` `account/rateLimits/read` response, falling back to the latest `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl` `token_count.rate_limits` event |

The API container mounts `~/.claude` read-only and `~/.codex` read-write from
the host. Codex needs write access so the app-server can use the normal Codex
auth and refresh flow. No secrets are embedded in the image.

---

## License

MIT
