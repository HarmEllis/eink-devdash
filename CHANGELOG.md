# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

> **Status:** the BW-panel and `SAFE_BW` work below is **experimental and
> pending Phase 0 hardware validation**. Gate 0.A (BW partial-refresh
> feasibility) and Gate 0.B (`SAFE_BW` non-destructiveness on BWR) results
> are recorded in `firmware/BOARD_NOTES.md` and must be PASS before this
> section is promoted to a tagged release. Until then, treat the entries
> as work-in-progress on `main` for early adopters running both panels.

### Added

- Experimental support for the WeAct Studio 2.9" Black/White (BW) SSD1680 panel alongside the existing Black/White/Red (BWR) panel out of the same firmware binary. The active variant is selected per-device in the provisioning portal under "Display" and persisted in NVS. On a BW panel the firmware folds red drawing operations to black at the framebuffer level and drives the controller with a BW-only refresh sequence (`EINK_REFRESH_BW_FULL`); on a BWR panel the existing BWR refresh (`EINK_REFRESH_FULL_COLOR`) is unchanged.
- Panel-agnostic `EINK_REFRESH_SAFE_BW` mode used by the provisioning recovery / setup-timeout paths, where the firmware does not yet know which panel is wired. Configured with `0x21=0x00,0x00` and the BWR `0x26` write skipped so the same waveform shows the QR / alert chrome on both panels, with BW-only icon redraws so the visuals are readable on BWR. The safety of this on the BWR panel is documented in the driver and is the explicit subject of Phase 0 Gate 0.B in `firmware/BOARD_NOTES.md`.
- NVS schema v3: `dash_config_v2_t` gains a trailing `panel_variant` byte. The v3 layout is byte-prefix-identical to v2, so v2 blobs are migrated in place on first load by reading the legacy bytes, validating the v2 CRC, defaulting `panel_variant` to BWR, and persisting as v3.
- Phase 0 BW-panel bring-up harness on the `phase0/harness-bw-29` branch (`firmware/main/phase0_harness.c`, `CONFIG_DEVDASH_PHASE0_HARNESS`) with Gate 0.A (BW partial-refresh feasibility) and Gate 0.B (`SAFE_BW` non-destructiveness on BWR) scenarios, plus a Phase 0 results section in `firmware/BOARD_NOTES.md`.
- `CONFIG_DEVDASH_DEMO_PANEL_VARIANT` Kconfig so demo builds (which skip NVS) can pick BWR or BW for the static demo dashboard.

### Changed

- `eink_init()` is now mode-agnostic: it performs hardware + software reset only and leaves the handle in `asleep=true`, so the first `eink_refresh()` programs the controller for whichever mode the caller actually asked for. This is what makes a single binary safe to drive both panel variants and `SAFE_BW` from cold boot.
- The display layer routes provisioning-recovery and setup-failed / setup-timeout draws through `EINK_REFRESH_SAFE_BW` and tracks the last refresh's panel variant in RTC memory so a variant change forces a full refresh on the next wake.

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

[0.2.0]: https://github.com/HarmEllis/eink-devdash/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/HarmEllis/eink-devdash/releases/tag/v0.1.0
