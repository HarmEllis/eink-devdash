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
```

Start the server:

```bash
docker compose up -d
```

The API is available at `http://<your-vm-ip>:3000`. The `/health` endpoint requires no authentication; all other routes require `Authorization: Bearer <DEVICE_TOKEN>`.

### 2. Firmware

Open the project in VS Code with the dev container, or install ESP-IDF v5.3 manually.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

On first boot the display shows a QR code. Scan it with the ESP SoftAP provisioning app (or any BLE/SoftAP provisioning tool) to set:

- WiFi SSID + password
- API URL (e.g. `http://192.168.1.50:3000`)
- Device token (must match `DEVICE_TOKEN` in `.env`)
- Refresh interval (3–60 minutes)

Configuration is saved to NVS. To re-provision, erase flash:

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

Namespace `dash` — all keys validated with CRC on boot.

| Key | Type | Description |
|-----|------|-------------|
| `schema_version` | u16 | NVS schema version |
| `api_url` | string | API server URL |
| `device_token` | string | Bearer token |
| `refresh_min` | u8 | Refresh interval (3–60 min) |
| `last_red_state` | u8 | Whether red ink was active last render |
| `bw_fast_cycle_count` | u8 | Cycles since last full-color refresh |
| `cap_profile` | blob | Cached capacity profile |

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
