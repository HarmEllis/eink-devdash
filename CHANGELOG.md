# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

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

[0.7.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/HarmEllis/eink-devdash/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/HarmEllis/eink-devdash/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/HarmEllis/eink-devdash/releases/tag/v0.1.0
