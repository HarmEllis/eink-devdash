# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

## [0.11.1] - 2026-07-03

This release fixes a rendering glitch on the 5H/7d usage bars where the
recency pattern reverted to solid red once it crossed the 80% alert
threshold.

### Fixed

- **Recency pattern in the alert-red zone**: the "last hour"/"today" recency
  pattern on the usage bars now keeps rendering once a column enters the
  alert-red (>80%) zone instead of falling back to a plain solid fill there,
  so the bar transitions monotonically (pattern -> red pattern) instead of
  reverting to solid mid-pattern. The README mockup generator mirrors the
  same fix.

## [0.11.0] - 2026-06-24

This release changes the recent usage slice on the 7-day bar to show today's usage instead of just the last hour, and fixes some rendering inconsistencies in the two-provider layout.

### Changed

- **Weekly bar recent slice**: the grey "recent usage" slice on the 7-day bar now uses the start of the local calendar day as its cutoff, showing how much of the weekly budget was consumed today. The shorter session (5h) window continues to show the last hour.
- Documented the Gemini (`agy`) tmux-pane review process in `AGENTS.md`.

### Fixed

- **Two-provider bar rendering**: fixed under-filled rendering of the recent usage slice on the taller bars used in the two-provider and hero layouts, and corrected uneven bar spacing so the 7-day tick dash no longer touches the extra-usage bar.

## [0.10.0] - 2026-06-22

This release adds two pacing aids to the provider usage bars: a last-hour activity slice that shows how much of each window's usage is recent, and a recommended daily-limit tick on the 7-day bar that helps spread the remaining weekly budget evenly across the remaining days.

### Added

- **Last-hour usage slice**: an in-memory usage history tracks how much of each window's usage accrued in the last hour, emitted as the additive `recentPercent` field. The firmware renders this recent portion as hollow boxes with top/bottom centre marks (older usage stays solid-filled). Warm-up falls back to the oldest sample so the slice appears within a couple of polls after a restart, and a window-reset guard avoids flagging a tumbling-window reset as recent activity.
- **Weekly daily-limit tick**: the 7-day usage bar can show a small dash at a recommended daily-limit position, configured with `WEEK_TICK_MODE` (`ceiling` marks today's ceiling — current usage plus an equal share of the remaining weekly budget over the remaining days; `even-pace` marks where usage should be by now if spread evenly across the week). `WORK_DAYS` restricts the calculation to the days you actually use your quota; working time is integrated continuously over the exact window rather than counted by calendar dates.

### Changed

- `docker-compose.yml` passes `WEEK_TICK_MODE` and `WORK_DAYS` through to the API container.
- The README colour key and environment-variable table document the new slice and tick, and the dashboard screenshot was regenerated to reflect them.

### Fixed

- Corrected a tick/extra-usage-row overlap in the 2-provider layout.

## [0.9.0] - 2026-06-14

This release introduces Antigravity as a new provider with live quota usage, refactors the firmware and API to a generic multi-provider usage grid supporting up to four providers, and fixes several display layout and bar-width inconsistencies.

### Added

- **Antigravity provider**: live quota consumption from the Antigravity API, split into separate tiles per quota group (Gemini and Claude/GPT). Requires `ANTIGRAVITY_API_KEY` in the API environment.
- **Generic multi-provider usage grid**: the firmware now renders up to four providers using hero (1), row (2), and grid (3–4) layout modes. An abstract icon field (`spark`, `ring`, `lift`, `diamond`, `generic`) drives the per-provider logo; no firmware rebuild is needed when the set of providers changes.
- **`getServices()` adapter method**: usage adapters can now emit multiple service tiles from a single provider entry; `buildDashboardPayload` flattens them and caps the combined tile count at four.

### Changed

- All named provider structs in the firmware (`claude`, `codex`, `antigravity`) are replaced by a generic `usage[4]` array; `parse_usage_service` reads any `kind:usage` tile by label, icon, and metric fields.
- `DISPLAY_RTC_STATE_MAGIC` is bumped so that a partial-refresh RTC cache from an older firmware build is invalidated automatically after an OTA update.
- The README dashboard screenshot now reflects the new multi-provider grid layout.

### Fixed

- Percentage bars now use a fixed right-edge anchor at the "100%" position, so all bars in a cell end at the same x coordinate and the right edge no longer jumps when the value text changes width (e.g. "9%" → "100%").
- The extra-usage (spend) bar uses the same fixed-anchor logic, preventing it from rendering wider than the percentage bars above it when the amount text is short.
- Header text is vertically centred (y offset corrected by 1 px) across all screens; icons were already at the correct position.
- The bottom-row cell gap in 2- and 4-provider layouts is now consistent with the top-row gap (both 5 px below the divider).

## [0.8.0] - 2026-06-13

This release adds an always-connected WiFi mode for USB/mains-powered installations and hardens heap management for long-running operation.

### Added

- **Always-connected mode**: the device can now stay awake between refreshes instead of deep-sleeping. While idle it holds its WiFi association and enables minimum-modem power save. A long BOOT press still opens the setup portal during idle waits. The option is enabled per device from the captive portal.
- **Quiet-hours behavior choice**: when always-connected mode is on, each network can independently choose whether quiet hours put the device into deep sleep (the original behavior) or keep it awake and associated while pausing API and display updates.

### Changed

- Quiet-hours status messages in the firmware now distinguish the new connected-pause state from the existing deep-sleep state.
- Heap allocations in the main loop and WiFi provisioning are released more aggressively between refresh cycles, and a periodic heap-low warning fires when free heap falls below 20 KB, guarding against slow leaks during long-running operation.

## [0.7.1] - 2026-06-12

This release recovers from transient WiFi/API failures within a single wake instead of immediately showing an error, and tidies the extra-usage bar so it never renders wider than the session/week bars above it.

### Fixed

- The extra-usage bar is now capped to the width of the session/week bars above it. A short amount previously left the bar extending further right than the other two; it now stays at most as wide as them and still shrinks when a wider amount needs the room.
- The dashboard no longer shows "NO WIFI"/"NO API" after a single transient
  failure on the production (deep-sleep) path. Each wake now retries the whole
  connect+fetch cycle up to three times, a scan that sees none of the
  configured networks is re-run once after a short back-off, and a configured
  network that was hidden behind a visible secondary is retried on all
  channels. An unambiguous permanent error (no API endpoint configured, or a
  missing device token) sleeps immediately instead of retrying, and a per-wake
  elapsed-time cutoff stops starting new retry cycles so a wake cannot keep
  retrying indefinitely. The relay fetch cycle also skips issuing a request
  when too little of its budget remains to complete one.

## [0.7.0] - 2026-06-11

This release adds live, locale-aware Claude extra-usage costs and an optional Codex overage indicator, improves Cloudflare relay connection monitoring, and makes WiFi and direct-API timeouts configurable from the captive portal.

### Added

- A currency-aware extra-usage row for Claude, populated live from OAuth usage credits with the amount consumed and percentage of the monthly cap. EUR and USD are supported, and `DASHBOARD_LOCALE` controls the decimal separator.
- Optional manual extra-usage amounts for Claude and Codex through `CLAUDE_OVERAGE_USD` and `CODEX_OVERAGE_USD`.
- The captive portal can now set the WiFi connect timeout (15-60 s, default 30) and the direct-API request timeout (5-20 s, default 10). Both are stored on the device, so a slow network can be given more time without rebuilding the firmware. The relay request timeout and the overall fetch-cycle budget are unchanged; with multiple API profiles a high API timeout can reduce how much failover fits in one refresh cycle.
- Application-level relay heartbeats with protocol-version reporting and configurable heartbeat intervals and timeouts.

### Changed

- Claude usage windows and extra-usage credits are now read from the free OAuth usage endpoint when available, with the existing billed probe retained only as a fallback for usage windows.
- The WiFi connect timeout is no longer a build-time setting; the firmware now reads the portal-configured value, with the default and bounds defined in one place.

### Fixed

- Half-open relay WebSocket connections are detected and reconnected instead of remaining silently unavailable.
- A Claude OAuth token rejected with HTTP 401 is forcibly refreshed even when its local expiry time has not yet passed.

## [0.6.0] - 2026-06-10

This release reworks the on-device factory reset into a clearer tap-to-reset / hold-to-erase gesture with confirmation screens, speeds up WiFi reconnection, and stores configuration as per-network NVS blobs that can never be partially overwritten. It also adds API-endpoint reordering to the captive portal and a relay identity reuse prompt to setup.

### Added

- Up and down reordering of API endpoints in the captive portal, so the dashboard's provider order can be arranged without re-entering the list.
- A prompt during relay setup to reuse this machine's existing identity or regenerate it, instead of always creating a new one.

### Changed

- The setup-mode factory reset is now a clearer two-gesture flow: a short BOOT-button tap resets the configuration while a long hold erases everything, each with an on-screen confirmation that can be cancelled and a result screen reporting success or failure.
- Configuration is now stored as per-network NVS blobs with guaranteed save headroom, using atomic bank-switch saves and a capacity preflight so a save can never partially overwrite a known-good configuration.
- WiFi reconnection is faster and less timeout-prone: once the device has scanned and ranked the configured networks by signal, it now associates directly with the chosen access point instead of letting the driver run a second, redundant all-channel scan. Strongest-AP selection across multiple access points on the same network is unchanged.

## [0.5.0] - 2026-06-09

This minor release adds a selectable WiFi region, a web-flasher-free factory reset gesture on the device, and relay improvements that let several devices share one Worker and reprint their provisioning QR codes on demand.

### Added

- A WiFi region selector in the captive portal that sets the regulatory domain (country code) independently of the rest of the configuration, defaulting to a world-safe value that ships anywhere.
- A device-side factory reset gesture using the BOOT button, requiring no web flasher: a long press in setup forgets the WiFi credentials, and a second press within ten seconds erases all stored configuration.
- A `npm run qr` relay command (with a Docker equivalent) that reprints the provisioning QR codes from the existing `.env` without redeploying the Worker or rotating credentials.
- Support for multiple relay setup identities, so several devices can share a single Worker, each authenticated with its own per-device secret.

### Changed

- ESP32-S3 SoftAP provisioning is more compatible: AMPDU RX/TX are disabled and the access point runs in 11b/g mode.
- The relay now authenticates each device with a per-device secret, and re-running setup creates or reuses this machine's identity instead of rotating a single shared credential.

## [0.4.1] - 2026-06-07

This patch release makes offline display refreshes and SoftAP provisioning more reliable, and reworks the relay so every device fetch is served on demand over an active WebSocket.

### Changed

- The relay no longer stores dashboard snapshots, publishes periodically, or bypasses cooldowns; every device fetch is now served on demand over an active WebSocket connection.
- The relay returns HTTP 503 when no publisher is available so firmware treats it as a transient error and retries.

### Fixed

- SoftAP provisioning now recovers reliably and reports explicit access point diagnostics.
- The display shows `NO WIFI` and `NO API` retry attempts while still honoring the configured partial and full e-ink refresh limits.
- The startup boot poster is shown again on boot.

## [0.4.0] - 2026-06-07

This minor release adds GitHub inbox notifications, a schema version 2 dashboard API, and an optional Cloudflare relay for secure remote updates without exposing the local API.

### Added

- GitHub inbox notifications on the dashboard, including unread pull request, issue, mention, and review-request summaries.
- A payload-agnostic Cloudflare Worker and Durable Object relay that lets devices fetch dashboard data and OTA manifests remotely over HTTPS.
- One-command relay setup for cloned repositories and a Docker-based setup path that does not require cloning the project.
- Provider adapter registries for usage and code-host services, making the schema version 2 dashboard API extensible without adding provider-specific logic to the relay.
- A one-minute refresh option for black/white panels when at least two partial refreshes are configured.

### Changed

- The dashboard API and firmware parser now use schema version 2 with normalized usage, code-host, alert, and metadata fields.
- Relay deployment runs only for release tags and skips cleanly when Cloudflare credentials are unavailable.
- OTA version selection and runtime policy handling are more robust for relay-backed devices.
- The README now focuses on installation, operation, relay setup, and contributor workflows.

### Fixed

- The dashboard route no longer fails when `DASHBOARD_TIME_ZONE` is present but empty.
- The captive portal accepts secure `https://` API profile URLs used by relay deployments.
- Relay setup preserves existing environment values and writes generated files using the host user's UID and GID.

## [0.3.1] - 2026-06-04

This patch release improves WiFi connection reliability on multi-AP networks by letting ESP-IDF roam across matching access points during connection attempts.

### Changed

- WiFi connection attempts now scan all channels, sort matching APs by signal strength, enable 802.11k/v roaming hints, and allow ESP-IDF driver-level retries instead of pinning a configured SSID to one scanned BSSID.
- Runtime WiFi station configuration is kept in RAM during reconnect attempts to avoid unnecessary hot-path writes to WiFi NVS.

## [0.3.0] - 2026-06-03

This minor release adds selectable WeAct 2.9" black/white panel support with faster partial refreshes, per-network quiet hours, and several dashboard and OTA reliability fixes.

### Added

- Support for the WeAct Studio 2.9" Black/White (BW) panel alongside the existing Black/White/Red (BWR) panel. The panel type is selected per device in the provisioning portal under "Display" and remembered across reboots and updates. On a BW panel, red content is drawn as black.
- Faster, lower-flicker dashboard updates on the BW panel: only the parts of the screen that actually changed are repainted between full refreshes, instead of redrawing the whole panel every cycle.
- "Max partial refreshes" setting in the provisioning portal (1–100, default 5): how many partial updates the BW panel does before forcing a full refresh. Lower values clear ghosting sooner; higher values refresh the whole panel less often, at the cost of more ghosting between updates.
- Per-network "quiet hours" in the provisioning portal: a local-time window (e.g. 23:00–06:00) during which the device skips its update cycle and just sleeps, saving battery and avoiding overnight panel wear when nobody is watching. The device shows the dashboard with a "SLEEPING / WAKES HH:MM" footer during the window and resumes automatically afterwards. Local time is taken from the dashboard API (the new `updatedAtLocalIso` field), so no clock or timezone needs to be set on the device. See `docs/decisions/0006-quiet-hours.md` for the design.

### Changed

- Existing device settings are migrated automatically when updating from an earlier firmware version: WiFi networks, API endpoints, and the refresh interval are preserved, as is a previously saved panel choice. Configs from the original firmware (which predate panel selection) adopt the build's default panel (BWR), which can be changed in the portal.
- Provisioning and recovery screens (QR code, connecting, setup-failed, setup-timeout) render correctly on both panel types and clear any leftover red on a BWR panel.

### Fixed

- BW partial refreshes continue to be used for normal alert changes instead of forcing unnecessary full-screen refreshes.
- Claude rate-limit probes are treated as full usage so the dashboard reports usage state accurately.
- The dashboard refresh interval header label is rendered correctly.
- OTA throttle counters, reconnect hints, and display refresh bookkeeping avoid unnecessary NVS writes by keeping deep-sleep-only state in RTC memory.

## [0.2.0] - 2026-05-27

This minor release adds end-to-end OTA update support for the ESP32-S3 firmware, hardens the API container's host-credentials story, and keeps the local flash-server in sync with the published OTA manifest.

### Added

- OTA update support: a `TWO_OTA` partition layout with 1.5 MB app slots, an `/ota/manifest` endpoint on the API that advertises the matching firmware tag, and a firmware-side `ota_client` that checks for, downloads, applies, and confirms updates with rollback on failed boot. See `docs/decisions/0005-ota-updates.md` for the design.
- Version-prefixed Docker tags so consumers can pin to `ghcr.io/harmellis/eink-devdash:0.2.0`, `0.2`, `0`, or `latest`.
- Pre-merge CI guardrails for OTA: every PR builds the firmware, asserts the partition table contains `ota_0` and `ota_1`, and fails if `eink-devdash.bin` would not fit in the 1.5 MB OTA slot.
- Release-assets pipeline: tagging `vX.Y.Z` now publishes `bootloader.bin`, `eink-devdash.bin`, `partition-table.bin`, `ota_data_initial.bin`, and `SHA256SUMS` to the GitHub Release alongside the hosted Web Flasher on Pages.
- `flash-server/sync-bins.sh`: a single source of truth that copies the binaries referenced by `manifest.json` into `bins/`, used by both local dev scripts and the Pages workflow.

### Changed

- The API container now runs as the host's UID/GID via `HOST_UID` / `HOST_GID` env vars instead of granting filesystem ACLs to UID 1000. `CODEX_HOME` moved to `/tmp/devdash-codex-runtime` so the runtime path stays writable for any UID.
- Claude OAuth tokens are now refreshed in place inside the API container so the dashboard stays live across token expiry without a restart cycle.
- `flash-server/serve.sh` and `flash-server/watch.sh` no longer hardcode the binary list; they delegate to `sync-bins.sh`, which reads `manifest.json`. This prevents drift between the OTA manifest and the local flasher.

## [0.1.0] - 2026-05-24

Initial public release of the e-ink developer dashboard: ESP32-S3 firmware for a WeAct 2.9" black/red display paired with a Node.js API container that exposes Claude and Codex CLI activity over the LAN.

[0.11.1]: https://github.com/HarmEllis/eink-devdash/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.10.0...v0.11.0
[0.10.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.7.1...v0.8.0
[0.7.1]: https://github.com/HarmEllis/eink-devdash/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/HarmEllis/eink-devdash/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/HarmEllis/eink-devdash/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/HarmEllis/eink-devdash/releases/tag/v0.1.0
