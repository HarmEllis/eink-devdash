# ADR-0005: OTA updates

- **Status:** Accepted
- **Date:** 2026-05-27

## Context

Today the ESP32-S3 firmware ships as a single-binary image flashed via the
webflasher (`flash-server/`). Any firmware change requires the device
owner to physically connect USB and reflash. The release pipeline already
produces signed binaries (`pages.yml` builds firmware on every `v*.*.*`
tag and publishes them as immutable GitHub Release assets), but devices
never see those binaries unless someone reflashes them.

This ADR captures the decisions for adding OTA so the device can pull
new firmware on its own after the first install.

## Decisions

### Partition layout — custom 2-slot CSV at 1.5 MB

`CONFIG_PARTITION_TABLE_CUSTOM=y` pointing at `firmware/partitions.csv`:

```
nvs,       data, nvs,     0x9000,   0x6000
otadata,   data, ota,     0xf000,   0x2000
phy_init,  data, phy,     0x11000,  0x1000
ota_0,     app,  ota_0,   0x20000,  0x180000   # 1.5 MB
ota_1,     app,  ota_1,   0x1A0000, 0x180000   # 1.5 MB
```

ESP-IDF v5.3's stock `partitions_two_ota.csv` ships with 1 MB OTA slots
plus a 1 MB factory partition. Once `esp_https_ota` and the mbedtls
cert bundle are linked in, our binary is ~1.07 MB, so the stock 1 MB
slots are too small. We drop the factory partition (we don't need a
"known-good fallback that never updates" since the rollback partition
already covers that) and enlarge each OTA slot to 1.5 MB. ~450 KB
headroom per slot.

The intent was the stock preset; carrying a custom CSV is the cheapest
way to keep the design otherwise unchanged. The CSV is minimal,
maps 1:1 onto IDF's defaults except for slot size, and is the single
source of truth for both the build and the webflasher manifest.

**Rejected alternative:** preserving NVS during the v0.1.x → first-OTA
migration. The ergonomic win (no re-provisioning on the one-time USB
install) was not worth making migration sensitive to the exact previous
flash state. Migration runs once per device.

**Migration consequence — locked.** The first OTA-capable install is a
webflasher migration that prompts the owner to erase the chip before
installing the new partition layout (`new_install_prompt_erase: true`).
This wipes saved Wi-Fi networks and the API token by design, even though
the new CSV keeps the NVS offset and size at the legacy values. The owner
re-runs the captive provisioning portal once, and every subsequent update
flows OTA without user involvement. A manual `esptool write_flash` without
`erase_flash` may preserve NVS, but that is not the supported migration
path.

### API ↔ firmware version coupling

The git tag `vX.Y.Z` is the single source of truth.

- `docker-publish.yml` passes `--build-arg APP_VERSION=${TAG}` so the
  API container has `process.env.APP_VERSION` available at runtime.
- `pages.yml` builds firmware on the tagged commit, so IDF's default
  `git describe`-based `PROJECT_VER` resolves to the clean `vX.Y.Z` and
  `esp_app_get_description()->version` reports it.
- The API advertises `APP_VERSION` as `latestVersion` from
  `/ota/manifest`.

A version mismatch between the API and the firmware build (e.g. someone
runs the API container at a non-tag) surfaces as
`{otaEnabled: false}` plus a warn log, never as a wrong download URL.

### `/ota/manifest` route

New route `GET /ota/manifest`, gated by the existing Bearer-auth hook.
Disabled via `OTA_ENABLED=false` (default `true`), mirroring the
`MDNS_ENABLED` pattern.

Enabled response:

```json
{
  "otaEnabled": true,
  "latestVersion": "v0.2.0",
  "downloadUrl": "https://github.com/HarmEllis/eink-devdash/releases/download/v0.2.0/eink-devdash.bin"
}
```

Disabled or `APP_VERSION` unset:

```json
{ "otaEnabled": false }
```

The owner/repo slug is a build-time constant in `api/src/routes/ota.ts`
— not env-driven — so a wrongly-deployed fork cannot silently advertise
a download URL that points at someone else's binaries.

**No external SHA256 manifest field.** Three reasons:

1. ESP-IDF embeds a SHA256 in every app image header and the bootloader
   verifies it on every boot. A truncated or tampered download fails to
   boot and the OTA rollback partition takes over.
2. TLS to `github.com` / `objects.githubusercontent.com` via the
   bundled Mozilla cert authority is the trust anchor in transit, and the
   firmware rejects manifest URLs whose authority is not one of those
   exact hosts.
3. Adding the field would couple `pages.yml` (produces the SHA) and
   `docker-publish.yml` (consumes the SHA) — the two workflows run in
   parallel today and we want to keep it that way.

If a defense-in-depth check is ever wanted, `SHA256SUMS` is already a
Release asset and the API can lazily fetch it. Punt unless demonstrated.

### On-device OTA flow

New component `firmware/main/ota_client.{c,h}` using `esp_https_ota`:

```
main.c app_main:
  ... existing connect/fetch path ...
  ota_client_maybe_update(cfg, network_idx, api_idx);
  display_render(&data);
  ota_client_mark_image_valid();        // commits PENDING_VERIFY → VALID
  enter_deep_sleep(cfg.refresh_min);
```

- **Manifest fetch.** HTTP or HTTPS to the configured API base URL with the
  existing Bearer token — same auth profile, HTTP stack, and cert bundle as
  `api_client.c::/dashboard`. A local LAN API uses plain HTTP (no new TLS
  surface on the LAN hop); a self-hosted HTTPS API verifies the server
  certificate via `crt_bundle_attach = esp_crt_bundle_attach`. A Cloudflare
  relay profile requests `/d/<uuid>/ota/manifest`; the Worker asks the API for
  a fresh manifest over the existing publisher WebSocket. The relay never
  caches manifests and returns 404 when no fresh answer is available. Firmware
  treats that 404 as a graceful skip.
- **Binary download.** HTTPS to GitHub Releases via
  `esp_https_ota_config_t` with `crt_bundle_attach = esp_crt_bundle_attach`
  and `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`. `esp_http_client` follows
  the 302 to `objects.githubusercontent.com` by default; the cert
  bundle covers both hosts. The firmware only accepts initial manifest
  download URLs whose authority is exactly `github.com` or
  `objects.githubusercontent.com`, case-insensitive and without userinfo.
- **Trust and version comparison.** The API accepts only canonical
  `vMAJOR.MINOR.PATCH` versions with uint32-bounded components and no leading
  zeros. Firmware pins the exact GitHub repository/tag/asset URL and installs
  only a strictly newer version. This blocks arbitrary download locations and
  downgrades. It does not authenticate the bytes at that path: firmware verifies
  no release signature, so binary authenticity rests on TLS and GitHub
  repository access control.
- **Throttle.** A single `RTC_NOINIT_ATTR uint32_t` counter (same
  pattern as `boot_button.c`'s force-prov magic). On any failure the
  counter is set to `DEVDASH_OTA_THROTTLE_CYCLES` (Kconfig, default 6),
  and each subsequent wake decrements it without contacting the
  network. Cold boot or a successful update zeroes it explicitly. At the
  default 5-minute refresh interval this is ~30 minutes of back-off.
- **Rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. The new
  image boots as `ESP_OTA_IMG_PENDING_VERIFY`; after the first
  successful dashboard fetch + render in the new image,
  `ota_client_mark_image_valid()` calls
  `esp_ota_mark_app_valid_cancel_rollback()`. On panic/reset before
  that point the bootloader reverts automatically.
- **Display integration.** `display_show_ota_update(from, to, slot,
  label)` paints one static full-refresh poster before flash erase/write
  starts. There are no progress, success, or failure redraws during OTA:
  the panel must not animate while the radio and flash controller are busy.
- **Full-refresh guard.** Every full-panel refresh goes through a central
  display helper that warns when two full refreshes are closer than
  `DEVDASH_WARN_FULL_REFRESH_INTERVAL_S` (default 180). It does not block
  the UX for minutes: occasional close refreshes are acceptable, while the
  normal OTA path avoids them by checking for OTA before rendering the
  dashboard and by never drawing progress/success/failure OTA frames. If
  the binary download fails after the OTA poster is already painted, the
  same wake can still repaint the dashboard; this is accepted for the rare
  failure path and is logged by the full-refresh guard.

### Release-workflow changes

- `docker-publish.yml` — pass `APP_VERSION` build-arg to the Docker
  build.
- `api/Dockerfile` — `ARG APP_VERSION` in both stages,
  `ENV APP_VERSION=${APP_VERSION}` in the runtime stage.
- `docker-compose.yml` — `OTA_ENABLED` defaults to `true` on both `api`
  and `api-mdns-host`.
- `ci.yml` — adds a `firmware` job that builds in
  `espressif/idf:release-v5.3`, asserts `eink-devdash.bin < 1.5 MB`,
  and asserts both `ota_0` and `ota_1` appear in the effective
  partition table.

`pages.yml` is unchanged; it already builds firmware, stages
`bootloader.bin` / `partition-table.bin` / `eink-devdash.bin` /
`SHA256SUMS`, creates the GitHub Release, and deploys
`flash-server/` to GitHub Pages with the manifest's `version` pinned
to the tag.

### Race window between Docker publish and GH Release

`docker-publish.yml` and `pages.yml` both start on the same tag push
and run in parallel. If a device polls `/ota/manifest` in the window
between the new Docker image becoming `latest` and the GitHub Release
being created, the download URL returns 404.

**Accepted as v1.** The OTA throttle converts the 404 into a single
failed cycle plus ~30 minutes of back-off — self-healing, user
invisible at typical refresh intervals, and the blast radius is a
single-user home deployment. If this ever actually annoys someone,
promote `merge-and-push` to gate on `gh release view "$TAG"` succeeding.

### IDF version pinning

Both the devcontainer (`.devcontainer/Dockerfile`) and the release
pipeline (`pages.yml`) pin ESP-IDF `v5.3`. The new `ci.yml` firmware
job pins the same. Bumping IDF must happen in all three together so a
green PR keeps mapping to a buildable release.

The CI and release jobs intentionally pin the ESP-IDF minor line rather
than a container digest. That keeps patch-level fixes within `v5.3`
available without a PR for every upstream image refresh; digest pinning
can be added if reproducibility becomes more important than patch uptake.

## Out of scope

- Firmware signing (secure boot v2 / signed app updates). Home use.
- Delta OTA (`esp_delta_ota`). The 1.5 MB slot is comfortable.
- A/B canary across devices. Single-device install.
- Push-style OTA from the API. Pull only.
- OTA over the captive provisioning portal. Station path only.
- Preserving NVS across the v0.1.x → first-OTA migration. Locked as
  erase + re-provision.

## Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Binary grows past the 1.5 MB OTA slot | CI guard fails the PR with a clear message |
| R2 | Mid-OTA power loss / Wi-Fi drop | `esp_https_ota` writes only the inactive slot; bootloader switches only on `esp_ota_set_boot_partition` at the end |
| R3 | Manifest advertises a valid HTTPS URL on an unexpected host | Firmware accepts only exact `github.com` / `objects.githubusercontent.com` hosts, with Mozilla cert validation |
| R4 | Existing devices lose stored credentials on migration | **Locked.** Webflasher erase prompt and documented one-time re-provisioning. |
| R5 | `latest` Docker tag races ahead of the GH Release | OTA throttle absorbs a transient 404 |
| R6 | `esp_https_ota` doesn't follow the 302 to `objects.githubusercontent.com` | Verified during first end-to-end integration; fallback is to resolve the redirect with `esp_http_client` and pass the absolute URL to `esp_https_ota` |
| R7 | A bad release leaves devices on a broken image | Bootloader rollback handles crash-loops; webflasher reflash is the escape hatch for "boots but unhappy" |
| R8 | Version compare wrong when minor/patch crosses 9→10 | Strict numeric semver parsing is host-tested, including `v0.9.0 < v0.10.0` and overflow rejection |
| R9 | Multiple display states request full refreshes in one wake cycle | OTA check runs before dashboard render when an update is available, progress redraws are forbidden, and `DEVDASH_WARN_FULL_REFRESH_INTERVAL_S` logs accidental close refreshes without blocking UX |
