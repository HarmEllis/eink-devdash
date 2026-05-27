# eink-devdash

A physical developer dashboard on a 2.9" black/red e-ink display, driven by
an ESP32-S3. Shows GitHub activity, Claude Code rate limits, and Codex usage,
updated on a configurable interval via deep sleep.

[![CI](https://github.com/HarmEllis/eink-devdash/actions/workflows/ci.yml/badge.svg)](https://github.com/HarmEllis/eink-devdash/actions/workflows/ci.yml)
[![Docker image](https://img.shields.io/badge/ghcr.io-eink--devdash-blue?logo=docker)](https://github.com/HarmEllis/eink-devdash/pkgs/container/eink-devdash)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

| Boot screen | Dashboard |
|-------------|-----------|
| <img src="docs/assets/readme-boot-screen.svg" alt="DevDash boot screen" width="420"> | <img src="docs/assets/readme-dashboard-screen.svg" alt="DevDash dashboard screen" width="420"> |

Red ink highlights alerts: Dependabot findings, usage above 80%, or auth
errors.

---

## Contents

- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Flashing the firmware](#flashing-the-firmware)
- [Provisioning over WiFi](#provisioning-over-wifi)
- [API reference](#api-reference)
- [Architecture](#architecture)
- [Development](#development)
- [Releases](#releases)
- [Technical reference](#technical-reference)
- [License](#license)

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

## Quick start

This is the recommended path for normal use: a published Docker image plus
the included web flasher. No build tools required.

### 1. Run the API server

Create a `.env` file next to `docker-compose.yml` (copy `.env.example` and
fill the values described in [Configuration](#configuration)).

Pull the latest published image and start the container:

```bash
docker compose pull
docker compose up -d
```

The API listens on `http://<host>:3000` and advertises
`http://devdash-api.local:3000` over mDNS when the container network allows
multicast.

> **Security:** keep this API on your trusted local network only. The
> firmware talks to it over plain HTTP and sends the bearer token without
> TLS, so do not expose port 3000 to the internet or route it through a
> public reverse proxy.

For reliable `.local` discovery on Linux Docker hosts, use host networking:

```bash
docker compose --profile mdns-host pull
docker compose --profile mdns-host up -d api-mdns-host
```

Pin a specific image version by setting `IMAGE_TAG` in `.env`:

```env
IMAGE_TAG=v0.1.0
```

Verify the API is reachable:

```bash
curl http://<host>:3000/health
# {"ok":true}
```

### 2. Flash the firmware

See [Flashing the firmware](#flashing-the-firmware). Browser-based flashing
is the fastest path; building from source is documented under
[Development](#development).

### 3. Provision

Power up the ESP32-S3, scan the on-screen QR with your phone, fill in WiFi
and the API URL/token in the captive portal. See
[Provisioning over WiFi](#provisioning-over-wifi).

---

## Configuration

Environment variables read by the API container. Set them in `.env` next to
`docker-compose.yml`.

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `DEVICE_TOKEN` | yes | — | Shared secret the firmware sends in `Authorization: Bearer …`. Generate 32+ random characters. |
| `GITHUB_TOKEN` | no | empty | Personal access token with `repo` + `security_events`. When empty the `github` block is omitted from `/dashboard`. |
| `CODEX_PLAN_TYPE` | no | empty | Set to `plus` or `team` when multiple ChatGPT accounts are visible. |
| `CODEX_LIVE_USAGE` | no | `true` | Set to `false` to skip the live Codex app-server probe and read only the on-disk session JSONL. |
| `CODEX_CLI_PATH` | no | empty | Override the Codex CLI binary path. |
| `CODEX_APP_SERVER_TIMEOUT_MS` | no | `8000` | Timeout for the Codex live probe. |
| `HOST_UID` | no | `1000` | UID the container runs as. Set to `$(id -u)` so the API can write `~/.claude/.credentials.json` as the host user that owns it. |
| `HOST_GID` | no | `1000` | GID the container runs as. Set to `$(id -g)`. |
| `CODEX_HOME` | no | `/tmp/devdash-codex-runtime` | Writable Codex runtime home inside the container. Set by `docker-compose.yml`. Lives under `/tmp` so it stays writable for any UID. |
| `CODEX_SOURCE_HOME` | no | `/home/node/.codex-source` | Read-only host Codex home mount used as the source for auth/config sync. Set by `docker-compose.yml`. |
| `CODEX_SESSIONS_DIR` | no | `/home/node/.codex-source/sessions` | Codex session JSONL directory used by the fallback reader. Set by `docker-compose.yml`. |
| `MDNS_ENABLED` | no | `true` | Set to `false` to disable mDNS advertising. |
| `MDNS_NAME` | no | `devdash-api` | Hostname under `.local`. |
| `IMAGE_TAG` | no | `latest` | Pin the published image to a specific tag (e.g. `v0.1.0`). |

The container mounts `~/.claude` **read-write** and mounts host `~/.codex`
read-only at `/home/node/.codex-source`. The Claude side needs write access
because the API refreshes the OAuth access token in place (surgical edit of
`claudeAiOauth.{accessToken,expiresAt,refreshToken}`, all other fields
untouched, atomic temp-file + rename) so the dashboard stays live while
Claude Code is idle. The Claude CLI keeps working transparently with the
refreshed credentials.

For the refresh path to work, the container must run as the **same UID/GID
as the host user that owns `~/.claude`**. Set `HOST_UID` and `HOST_GID` in
`.env`:

```bash
echo "HOST_UID=$(id -u)" >> .env
echo "HOST_GID=$(id -g)" >> .env
```

`docker-compose.yml` passes these to the container via the `user:`
directive, so the API writes credentials as your own host user — no `chown`
or ACL on `~/.claude` required, and no extra principal gains access. If
`HOST_UID`/`HOST_GID` are unset the container defaults to `1000:1000`,
which only works when your host user happens to be uid `1000`. If the UIDs
don't match the api logs `cannot write to /home/node/.claude` and falls
back to the read-only path; the dashboard still works whenever the on-disk
token is fresh.

Run **either** the default `api` service **or** the host-network
`api-mdns-host` profile, not both at once: the credentials write path is
guarded by a file lock so concurrent api processes won't corrupt the file,
but two services serving the dashboard simultaneously isn't an intended
deployment.

Before each live Codex usage probe, the API syncs auth/config files from
the Codex source into `/tmp/devdash-codex-runtime` (writable for any UID)
used by `codex app-server`. Session JSONL fallback reads directly from
`/home/node/.codex-source/sessions`, so new host sessions are visible
without copying session history into the container. No keys are baked into
the image.

### Network security

The API is designed for a trusted local network only. Do not expose it to
the internet, publish port 3000 through a router, or put it behind a public
reverse proxy. The firmware currently uses plain HTTP, so the
`Authorization: Bearer ...` token protects against accidental LAN access but
does not provide confidentiality on untrusted networks. If the device is not
on the same trusted LAN as the API, provide that private network path outside
the firmware, for example with a router/site-to-site VPN or another private
network gateway. The firmware itself does not run a VPN client.

---

## Flashing the firmware

Use the web flasher for normal flashing. The devcontainer is for building
the firmware; it is not expected to have access to the host's USB serial
devices, so direct USB flashing with `idf.py` is not the supported workflow
there.

### Option A — Web flasher (recommended)

A static page flashes a pre-built `.bin` via the Web Serial API in Chrome
or Edge — no local toolchain required.

1. Plug the ESP32-S3 in over USB.
2. Open **<https://harmellis.github.io/eink-devdash/>** in Chrome or Edge
   on desktop (Web Serial is not supported on Firefox, Safari, or any
   mobile browser).
3. Click **Install** and select the device's serial port.
4. After flashing the page redirects to `flashed.html`, which mirrors the
   on-device provisioning instructions and includes a troubleshooting
   accordion.

The hosted version is pinned to the latest `v*.*.*` release. To flash a
locally-built firmware instead, host the `flash-server/` directory
yourself — see [Development → Web flash server](#web-flash-server).

### Option A.1 — Command-line flash via release assets

Each `v*.*.*` release publishes the three `.bin` files plus a
`SHA256SUMS` file as GitHub Release assets. Verify and flash with
`esptool.py` only from a host environment that has direct USB access:

```bash
TAG=v0.1.0
mkdir -p bins && cd bins
gh release download "$TAG" --repo HarmEllis/eink-devdash \
  --pattern '*.bin' --pattern 'SHA256SUMS'
sha256sum -c SHA256SUMS
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 eink-devdash.bin
```

### Option B — Build locally, flash with the local web flasher

For developers building firmware from source. See
[Development → Firmware](#firmware-development) for the devcontainer
setup. Build inside the devcontainer, then serve the generated binaries
through the local web flasher:

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
cd ../flash-server
bash serve.sh
```

Open `http://localhost:8080` in Chrome or Edge on the host machine and
click **Install**. The browser, not the devcontainer, talks to the ESP32-S3
over Web Serial. To wipe stored credentials, use the erase prompt in the
web flasher before installing.

---

## Provisioning over WiFi

On first boot — and whenever you long-press the BOOT button — the display
shows a SoftAP screen with a QR code, the AP SSID, the AP password, and the
captive portal URL `192.168.4.1`.

1. **Scan the QR with your phone camera.** Both iOS Camera and Android
   Camera / Google Lens parse the `WIFI:T:WPA;S:devdash-XXXX;P:…;;`
   payload and offer one-tap join.
2. **The captive portal pops up automatically** on iOS, Android, and
   Windows. If it does not appear, open `http://192.168.4.1` in a browser
   while still joined to the AP.
3. **Edit the form.** Up to five WiFi networks × five API endpoints each.
   Empty password/token fields mean "keep the saved value"; tick the
   matching *Clear* checkbox to erase a stored secret.
4. **Save.** The device confirms, reboots after ~4 s, joins WiFi, fetches
   the API, and renders the dashboard.

The AP password is a 12-character random string generated on first boot
and persisted in NVS. The same password survives reboots; a factory reset
from the web flasher regenerates it. API URLs must start with `http://`
— IP, DNS, and `.local` mDNS hostnames are all accepted. HTTPS is out of
scope for this firmware revision.

| Field | Notes |
|-------|-------|
| API URL | `http://192.168.1.50:3000` or `http://devdash-api.local:3000` |
| Device token | Must match `DEVICE_TOKEN` in the API server's `.env` |
| Refresh interval | 3–60 minutes, default 5 |

### BOOT button

| Action | When | Result |
|--------|------|--------|
| Short press | Deep sleep | Wake and refresh immediately |
| Long press (~5 s) | Any state | Enter the captive portal |

`CONFIG_DEVDASH_BOOT_LONGPRESS_MS` (default `5000`) is the single source of
truth for the long-press threshold.

> **Known limitation:** holding BOOT *while* applying USB power
> (cold boot) puts the ESP32-S3 into ROM download mode at the hardware
> level, before firmware runs. Power on first, then long-press BOOT.

If none of the stored WiFi networks are reachable, the device shows a
`NO WIFI` failure poster with the saved SSIDs it tried. If WiFi connects
but every configured API endpoint fails, it shows `NO API` with the
configured upstreams for the active network. The device then returns to
deep sleep, or keeps retrying when
`CONFIG_DEVDASH_RETRY_FOREVER_WHEN_OFFLINE=y`. Stored credentials are
never erased automatically.

---

## API reference

### `GET /dashboard`

Returns current dashboard data. Requires `Authorization: Bearer <token>`.

- The `github` object is omitted when `GITHUB_TOKEN` is unset or empty.
- Codex live usage is read through `codex app-server` first; the API
  falls back to the latest `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`
  `token_count.rate_limits` event when the CLI, auth, or live endpoint
  is unavailable.
- Set `CODEX_LIVE_USAGE=false` to disable the live probe.

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

Returns `{ "ok": true }`. No authentication required. Use it for container
health checks.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Docker host (Proxmox VM, NAS, Pi, …)                       │
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
│  ├── NVS: WiFi profiles + API endpoints + refresh interval  │
│  ├── WiFi provisioning (SoftAP + QR on first boot / hold)   │
│  ├── HTTP fetch → parse JSON → render to e-ink              │
│  └── Deep sleep between refreshes                           │
└─────────────────────────────────────────────────────────────┘
```

---

## Development

### Devcontainer (recommended)

The repo ships with a VS Code devcontainer ([`.devcontainer/`](.devcontainer/))
that pins ESP-IDF v5.3, Node, and the firmware/API toolchain. Open the
folder in VS Code and choose **Reopen in Container** — the post-create
step installs the API dependencies and the integrated terminal lands in a
shell with `idf.py`, `node`, and `npm` ready to go.

Without the devcontainer you need to install ESP-IDF v5.3 and Node 20+
yourself; everything below assumes you are running inside the container's
integrated terminal.

### Repo layout

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
├── docs/
│   └── decisions/          # Architecture decision records
└── .devcontainer/          # VS Code dev container (ESP-IDF)
```

### README screen previews

The screen previews at the top of this README are generated from the
firmware pixel font and mirrored display coordinates. Regenerate them after
display layout changes with:

```bash
node scripts/render-readme-screens.mjs
```

### API development

```bash
cd api
npm install
npm run dev    # tsx watch src/index.ts
```

Build the image locally instead of pulling from GHCR:

```bash
docker compose build api
docker compose up -d api
```

### Firmware development

From the devcontainer's integrated terminal:

```bash
cd firmware
idf.py set-target esp32s3   # once after a clean checkout
idf.py build
cd ../flash-server
bash serve.sh
```

Open `http://localhost:8080` in Chrome or Edge on the host machine and
flash with Web Serial. The devcontainer builds the firmware, but the
browser on the host owns USB access.

See [AGENTS.md](AGENTS.md) for the headless / agent-driven workflow that
drives the same container over `docker exec`.

### Web flash server

`flash-server/watch.sh` serves the bins on `http://localhost:8080` and
re-publishes them whenever `idf.py build` produces a new artifact:

```bash
cd flash-server
bash watch.sh
```

`serve.sh` does a single copy + serve and needs a restart after every
rebuild.

---

## Releases

Tags of the form `vMAJOR.MINOR.PATCH` (for example `v0.1.0`) trigger two
workflows in parallel.

[`.github/workflows/docker-publish.yml`](.github/workflows/docker-publish.yml):

1. Verifies the `CI` workflow succeeded on the tagged commit.
2. Builds the API container for `linux/amd64`.
3. Pushes the image to
   `ghcr.io/harmellis/eink-devdash` with tags
   `<version>`, `<major>.<minor>`, `<major>`, and `latest`
   (the `latest` tag is skipped for pre-release tags).
4. Signs the manifest with cosign (keyless, OIDC).
5. Runs Trivy and uploads SBOM artifacts.

[`.github/workflows/pages.yml`](.github/workflows/pages.yml):

1. Builds the firmware for `esp32s3` inside `espressif/idf:release-v5.3`.
2. Uploads `bootloader.bin`, `partition-table.bin`, `eink-devdash.bin`,
   and `SHA256SUMS` as assets on the GitHub Release for the tag (the
   release is created with `--generate-notes` if it does not exist).
3. Deploys the [`flash-server/`](flash-server/) static page plus the same
   binaries to GitHub Pages, with the release tag injected into both the
   page header and `manifest.json`.

The hosted flasher lives at <https://harmellis.github.io/eink-devdash/>
and is byte-identical to the Release-asset firmware.

> One-time repo setup before the first Pages deploy: **Settings → Pages →
> Build and deployment → Source: GitHub Actions.**

The release flow is therefore:

```bash
git commit ...
git push origin main          # wait for CI to pass on this exact SHA
git tag v0.1.0
git push origin v0.1.0
```

If `docker-publish` fails the CI-gating check, push the release commit
first, wait for CI, and re-tag.

---

## Technical reference

### Refresh strategy

| Mode | Duration | Trigger |
|------|----------|---------|
| BW fast monochrome | ~2–4 s | Dashboard metrics changed while both the previous and current frame are black/white-only |
| Full 3-color (Mode 1 LUT) | ~15–27 s | Red content changed, previous frame had red, first render, controller wake, or after ten fast monochrome refreshes |

Timestamp and reset-countdown changes by themselves do not repaint the
panel. Minimum refresh interval: 3 minutes (configurable 3–60 min).

> The BW partial-refresh path is currently disabled at compile time
> (`DISPLAY_ENABLE_BW_EXPERIMENT 0`) pending hardware validation — see
> [`docs/decisions/0003-red-free-bw-partial-refresh.md`](docs/decisions/0003-red-free-bw-partial-refresh.md).

### NVS layout

Namespace `devdash`.

| Key | Type | Description |
|-----|------|-------------|
| `cfg_v2` | blob | WiFi profiles, API endpoints, refresh interval |
| `ap_password` | string | Persisted SoftAP password |

Refresh-cycle bookkeeping (`bw_fast_cycle_count`, `last_red_state`) lives
in RTC slow memory and survives deep sleep but resets on power-on.

### Data sources

| Source | How |
|--------|-----|
| GitHub | REST API v3 via PAT (`repo` + `security_events` scopes) |
| Claude Code | Reads `~/.claude/.credentials.json` (OAuth token) for rate-limit headers and refreshes the access token in-place when it expires, so the dashboard stays live during long idle periods. No Anthropic API key required. |
| Codex | Live `codex app-server` `account/rateLimits/read` response, falling back to the latest `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl` `token_count.rate_limits` event |

---

## License

MIT — see [LICENSE](LICENSE).
