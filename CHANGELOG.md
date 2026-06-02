# Changelog

All notable changes to this project are documented in this file.

## [Unreleased]

> **Status:** Phase 0 hardware gates are recorded in `firmware/BOARD_NOTES.md`.
> Gate 0.A (BW partial-refresh feasibility) **passed**; Gate 0.B (panel-agnostic
> `SAFE_BW` recovery) **failed on a red-preconditioned BWR panel**, so the
> recovery design switched to a build-stamped panel SKU with serial/BOOT
> overrides (below). On-hardware flash verification of the per-region partials
> and the BWR recovery red-clear is the remaining step before this section is
> promoted to a tagged release.

### Added

- Support for the WeAct Studio 2.9" Black/White (BW) SSD1680 panel alongside the existing Black/White/Red (BWR) panel out of the same firmware binary. The variant is selected per-device in the provisioning portal under "Display" and persisted in NVS. On a BW panel the firmware folds red drawing operations to black and drives the controller with BW-only refresh sequences; on a BWR panel the existing `EINK_REFRESH_FULL_COLOR` refresh is unchanged.
- Per-region partial refresh on the BW panel (Phase 0 Gate 0.A passed): the dashboard is diffed against the last frame per region (Layout A = 6 regions with GitHub, Layout B = 3 regions without) and only changed regions are repainted with a narrow-X partial (BW V2 partial waveform LUT `0x32` + update trigger `0xCC`, geometry `RAMX=1..16`), capped at 5 partials per region between full refreshes. Any change outside the active region union, a layout flip, or the 24 h ghost-clear cap forces a `BW_FULL`. Shared state is committed only after every partial in a render succeeds.
- Build-stamped panel SKU `CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT` (Kconfig `int`, `range -1 1`, `default -1` so a SKU build fails the build closed if left unset; the repo dev/CI build sets `0` = BWR in `sdkconfig.defaults`). This is the Gate 0.B fallback: the panel variant is known before the first draw, so recovery surfaces use the correct refresh instead of the panel-agnostic `SAFE_BW` path that could not clear pre-existing red on a BWR panel.
- Cold-boot-only wrong-SKU recovery overrides: send `B` (BW) / `R` (BWR) over USB-Serial-JTAG, or hold the BOOT button through the boot window (→ BW), within the first few seconds of a cold boot. The override applies to that one boot only; the override window never runs on a deep-sleep wake, so battery refresh wakes are not delayed.
- NVS schema v3: `dash_config_v2_t` gains a trailing `panel_variant` byte. The v3 layout is byte-prefix-identical to v2, so v2 blobs are migrated in place on first load by reading the legacy bytes, validating the v2 CRC, seeding `panel_variant` from the SKU default (`CONFIG_DEVDASH_DEFAULT_PANEL_VARIANT`), and persisting as v3. A genuine saved v3 `panel_variant` always wins over the build default.
- Phase 0 BW-panel bring-up harness on the `phase0/harness-bw-29` branch (`CONFIG_DEVDASH_PHASE0_HARNESS`) with Gate 0.A / Gate 0.B scenarios, plus a Phase 0 results section in `firmware/BOARD_NOTES.md`.
- `CONFIG_DEVDASH_DEMO_PANEL_VARIANT` Kconfig so demo builds (which skip NVS) can pick BWR or BW for the static demo dashboard.

### Changed

- `eink_init()` is now mode-agnostic: it performs hardware + software reset only and leaves the handle in `asleep=true`, so the first `eink_refresh()` programs the controller for whichever mode the caller actually asked for. This is what makes a single binary safe to drive both panel variants from cold boot.
- Provisioning / recovery surfaces (QR, connecting, wait, setup-failed, setup-timeout offline) now render through the variant-aware `display_full_refresh()` — `FULL_COLOR` on BWR (which drives the red plane and clears any prior red) and `BW_FULL` on BW — instead of `EINK_REFRESH_SAFE_BW`. `EINK_REFRESH_SAFE_BW` / `display_full_refresh_safe()` are retained but dormant.

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
