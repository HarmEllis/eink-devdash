# Plan: per-network NVS blobs + guaranteed free-space headroom

## Goal

Replace the single ~7.2 KB `cfg_v2` NVS blob with one small blob per WiFi
network plus a tiny meta blob, and write changed blobs **one at a time** so a
re-save only ever needs free-space headroom for ONE network blob (a WiFi
profile with its 5 APIs), not for the whole config. Document and enforce that
headroom so `ESP_ERR_NVS_NOT_ENOUGH_SPACE` on a portal re-save (seen on
v0.5.0+PR#27) cannot recur with the maximum 5×5 config.

## Background (verified in code)

- `nvs` partition: 0x9000 + 0x6000 = 24 KB = 6 × 4 KB pages; NVS reserves one
  page for GC → 5 data pages × 126 entries × 32 B = 20 160 B usable
  (`firmware/partitions.csv`; the partition must not move/grow — partition
  table changes do not propagate via OTA).
- `storage_save_v2()` rewrites the whole fixed-size `dash_config_v2_t`
  (~7.2 KB) on every save. Overwriting an NVS blob needs old + new copies to
  coexist (~14.4 KB), alongside `nvs.net80211`, `ap_pwd`, `wifi_cc`. That only
  fits with near-perfect page packing; fragmentation tips it into
  `ESP_ERR_NVS_NOT_ENOUGH_SPACE`.
- `storage.h:31-36` already prescribes the fix: "switch to per-network blobs
  instead of widening this cap."

## New NVS layout (config version 6)

All keys stay in the existing `devdash` namespace.

| Key | Content | Size |
|-----|---------|------|
| `cfg_meta` | `dash_meta_blob_t`: version, caps, refresh_min, network_count, panel_variant, max_partials, write_counter, crc32 | 16 B |
| `cfg_net0`…`cfg_net4` | `dash_net_blob_t`: version, crc32, one `dash_wifi_profile_t` (profile + its 5 APIs + its quiet-hours fields) | ~1 440 B |

Struct changes in `storage.h`:

- Fold the v5 trailing parallel quiet-hours arrays into
  `dash_wifi_profile_t` (`quiet_enabled`, `quiet_start_min`, `quiet_end_min`
  per network). The parallel arrays existed only to keep the v2–v5 single-blob
  layout byte-compatible; per-network blobs remove that constraint.
  Consumers updated: `main.c` (quiet window check) and `wifi_prov.c`
  (portal render + form apply).
- `dash_config_v2_t` stays the in-RAM aggregate (callers keep their API), but
  drops `crc32` (CRCs now live per stored blob; nothing outside storage.c
  reads it — verified) and keeps `write_counter` mirrored from the meta blob.
- The exact v2–v5 legacy layouts move into private structs in `storage.c`
  (`dash_wifi_profile_legacy_t` + legacy config structs) used only by the
  migration read path; the existing offset/size static asserts move with
  them.

## Save algorithm (`storage_save_v2`) — one blob at a time

1. Normalize the cfg; sync the RTC last-success hints (unchanged semantics,
   storage.c:557-564).
2. Open NVS read-write; allocate two ~1.5 KB heap scratch buffers (candidate
   blob + stored blob — never on the caller's stack).
3. **Erase first**: `nvs_erase_key` + commit for every `cfg_net{i}` slot with
   `i >= network_count` (frees entries before any write) and for the legacy
   `cfg_v2` key if it still exists.
4. **Per-network, sequentially**: for each `i < network_count`, build the
   candidate blob, read the stored `cfg_net{i}` and `memcmp`. Identical →
   skip (no flash write, no wear). Different/absent → `nvs_set_blob` +
   `nvs_commit` for THIS blob only, then move to the next. Peak coexistence
   is therefore old+new of a single ~1.44 KB blob (~2.9 KB), regardless of
   how many networks changed. A mid-sequence failure leaves every blob
   individually valid (each carries its own CRC); a retry skips the
   already-written ones — the sequence is idempotent.
5. **Meta last**: if any network blob changed, a slot was erased, or the meta
   fields differ from the stored meta, write `cfg_meta` with
   `write_counter + 1`. A fully unchanged save writes nothing at all
   (today it rewrites 7.2 KB even for a no-op).
6. Runtime guard: before step 4, `nvs_get_stats()`; if
   `total_entries - used_entries < DASH_NVS_MIN_FREE_ENTRIES` (worst-case
   single network blob + meta + slack), fail early with
   `ESP_ERR_NVS_NOT_ENOUGH_SPACE` so the portal shows the existing clear
   error before any partial write — defense-in-depth, statically this cannot
   happen (see below).

Crash-consistency trade-off (documented in storage.c): the old single blob
was atomic; per-network blobs mean a power cut mid-save can persist a mix of
old and new networks. Every blob is individually CRC-valid, the load path
drops only broken blobs, and a portal re-save converges — accepted for this
device class in exchange for the guaranteed headroom.

## Load + migration (`storage_load_v2`)

1. If `cfg_meta` exists and validates (version 6, caps, CRC): read each
   `cfg_net{i}` for `i < network_count`, validate per-blob (version, CRC,
   ranges), copy into the RAM struct. An invalid/missing network blob is
   logged and skipped (the other networks survive — the corruption-blast-
   radius win storage.h already called out), then normalize.
2. Else fall back to the legacy `cfg_v2` key with the existing v2/v3/v4/v5
   length+version discrimination (now reading into private legacy structs on
   the heap), convert to the new layout, then migrate: `storage_save_v2()`
   (writes v6 blobs), erase the `cfg_v2` key, and erase the now-dead
   `nvs.net80211` namespace (see companion below).
   - Space strategy: try the v6 save first (old 7.2 KB blob + new ~7.3 KB in
     blobs both present — fits the 20 160 B budget but may fail on a
     fragmented partition). On `ESP_ERR_NVS_NOT_ENOUGH_SPACE`, erase the
     `cfg_v2` key first (config is in RAM) and retry once. The retry path has
     a power-loss window (config only in RAM until the save lands); accepted
     because it only runs on the already-broken-today fragmented case, and
     the alternative (no retry) bricks the migration entirely.
   - A migration save failure is non-fatal, as today: log, keep the
     in-memory cfg, next boot retries.

## Companion: WiFi driver storage → RAM (Option 1 — INCLUDED)

`wifi_net_init()` switches `esp_wifi_set_storage(WIFI_STORAGE_FLASH)` →
`WIFI_STORAGE_RAM`. The app owns all credentials in cfg and reconfigures the
driver from it on every boot/wake (`wifi_roam.c` already runs with
`WIFI_STORAGE_RAM`); the `nvs.net80211` namespace is dead weight in the same
24 KB partition. With RAM storage, `esp_wifi_set_country_code()` also stops
writing flash (we persist the country ourselves under `wifi_cc` and re-apply
each boot). The v5→v6 migration erases the `nvs.net80211` namespace once to
reclaim its entries; `storage_erase()` (factory reset) keeps wiping the
whole partition as before.

## Free-space requirement (documented + enforced)

Definitions (storage.h):

- `DASH_NET_BLOB_MAX_BYTES 1536` — hard ceiling for one network blob.
- An N-byte blob costs `ceil(N/32) + 3` entries worst case (blob index +
  chunk overhead). One network blob ≤ 51 entries; meta ≤ 4.
- `DASH_NVS_MIN_FREE_ENTRIES 64` — headroom that must remain free with the
  maximum config saved: one network blob re-write (old+new coexistence is
  per-blob thanks to the sequential save) + meta + slack.

Static asserts (storage.c):

- `sizeof(dash_net_blob_t) <= DASH_NET_BLOB_MAX_BYTES`,
  `sizeof(dash_meta_blob_t) <= 32`.
- Worst-case occupancy fits with headroom:
  `5 × entries(net) + entries(meta) + MISC + DASH_NVS_MIN_FREE_ENTRIES
   <= 5 pages × 126 entries`, where MISC = 32 entries reserved for
  `ap_pwd`, `wifi_cc`, namespace bookkeeping, and slack.
  (Numerically: 5×51 + 4 + 32 + 64 = 355 ≤ 630 — comfortable.)

AGENTS.md "NVS writes and RTC memory" gets updated: per-network blobs, the
headroom rule, and the instruction to recompute the asserts when caps grow.

## Out of scope

- Partition table changes (explicitly forbidden — OTA cannot move them).
- PR #27 content (this branch is from `main`; #27 remains open separately).
- Portal UI changes.

## Verification

- `idf.py build` in the devcontainer; binary < 1.5 MB OTA slot.
- Static asserts compile-time enforce the budget.
- Codex implementation review via co-dev; iterate until clean.
- On-device (maintainer): provision max config, re-save repeatedly, confirm
  no `ESP_ERR_NVS_NOT_ENOUGH_SPACE`; OTA from v5 build → v6 migration keeps
  all networks/APIs/quiet hours/panel settings.
