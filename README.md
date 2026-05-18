# eink-devdash

A physical developer dashboard on a 2.9" black/red e-ink display, driven by an ESP32-S3. Shows GitHub activity, Claude Code rate limits, and Codex usage — updated on a configurable interval via deep sleep.

```
┌─────────────────────────────────┐
│  GitHub          Claude Code    │
│  Issues: 3       5h: 42/50      │
│  PRs:    1       Week: 210/1000 │
│  Alerts: 0  ███  Codex: 12/100  │
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
│  │  └── Codex service   (OpenAI usage API)              │   │
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
| BW fast (Mode 2 LUT) | ~2–4 s | Every normal cycle |
| Full 3-color (Mode 1 LUT) | ~15–27 s | Red content changed, or every 10th BW cycle, or 24 h |

Minimum refresh interval: 3 minutes (configurable 3–60 min).

---

## Getting started

### Prerequisites

- Docker + Docker Compose (for the API server)
- [ESP-IDF v5.3](https://docs.espressif.com/projects/esp-idf/en/v5.3/) or the included dev container
- A GitHub Personal Access Token with `repo` and `security_events` scopes
- An OpenAI API key (for Codex usage stats)

### 1. API server

Create a `.env` file in the repo root:

```env
GITHUB_TOKEN=ghp_...
OPENAI_API_KEY=sk-...
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

On first boot the display shows a provisioning prompt. You have two ways to
hand the device your WiFi credentials:

**Option A — Improv Serial (browser, USB):**
After flashing in the web flasher (see *Web flash* below), the page offers a
*Configure Wi-Fi* step over the same USB connection (Improv Serial protocol).
Enter your SSID + password and the device connects and reboots into normal
mode — no app needed.

**Option B — SoftAP provisioning portal:**
Join the SoftAP advertised by the device with the *ESP SoftAP Provisioning*
app and supply your WiFi SSID + password. The SSID and the Proof-of-Possession
(PoP) are derived per device from the factory MAC and shown on the e-ink
display, e.g. `devdash-A4F2` / pop `aabbccddeeff`. Credentials are stored in
NVS by the ESP-IDF provisioning manager.

Both options run in parallel during the provisioning window — use whichever
fits your setup.

The API base URL, device bearer token, and refresh interval have build-time
defaults (`idf.py menuconfig` -> *DevDash Configuration*) and are migrated into
the runtime `cfg_v2` profile store on first boot. After that, manage up to five
WiFi networks and up to five API endpoints per network from the web flasher over
Improv Serial. API URLs must use `http://`; IP addresses, DNS names, and
`.local` mDNS names are accepted.

- API URL: `http://192.168.1.50:3000`
- Device token: empty (firmware will display `OFFLINE` until you set one)
- Refresh interval: 5 minutes

You can also commit your values to `firmware/sdkconfig.defaults` so they are
applied to every fresh build. The device token must match `DEVICE_TOKEN` in
the API server's `.env`.

If none of the stored WiFi networks are reachable, the device displays
`OFFLINE` and returns to deep sleep. It does not erase stored credentials
automatically. Hold the BOOT button while the device is asleep to wake it into
the provisioning and Improv configuration window.

To force re-provisioning manually anyway:

```bash
idf.py erase-flash
```

### 3. Web flash (optional)

Pre-built binaries can be flashed directly from a browser via the included web flash server. Use `watch.sh` (recommended) — it starts the HTTP server and automatically updates the served binaries whenever a new build completes:

```bash
cd flash-server
bash watch.sh
```

Open `http://localhost:8080` in Chrome and click Install. After each `idf.py build` the bins are refreshed automatically; just click Install again to flash the new firmware. No need to restart the server.

> `serve.sh` still works for a one-shot copy + serve, but requires a manual restart after every rebuild.

---

## API reference

### `GET /dashboard`

Returns current dashboard data. Requires `Authorization: Bearer <token>`.

```json
{
  "schemaVersion": 1,
  "github": {
    "issues": 3,
    "prs": 1,
    "dependabot": 0
  },
  "claude": {
    "fiveHour": { "used": 42, "limit": 50, "resetInSeconds": 1823 },
    "weekly":   { "used": 210, "limit": 1000, "resetInSeconds": 302400 },
    "authError": false
  },
  "codex": {
    "dailyUsed": 12,
    "dailyLimit": 100
  },
  "updatedAt": "2026-05-16T14:32:00Z"
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

WiFi credentials are stored separately by the IDF provisioning manager
(`nvs.net80211` namespace). The refresh-cycle bookkeeping (`bw_fast_cycle_count`,
`last_red_state`) lives in RTC slow memory and persists across deep-sleep
wakeups but resets on power-on.

---

## Data sources

| Source | How |
|--------|-----|
| GitHub | REST API v3 via PAT (`repo` + `security_events` scopes) |
| Claude Code | Reads `~/.claude/.credentials.json` (OAuth token) for rate-limit headers — no Anthropic API key required |
| Codex | OpenAI `/v1/organization/usage/completions` (OPENAI_API_KEY) |

The API container mounts `~/.claude` and `~/.codex` read-only from the host so no secrets need to be embedded in the image.

---

## License

MIT
