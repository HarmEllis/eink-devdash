# Plan: per-network NVS blobs + guaranteed free-space headroom

## Goal

Replace the single ~7.2 KB `cfg_v2` NVS blob with one small blob per WiFi
network plus a tiny meta blob. Write changed profiles into alternate bank
keys and atomically switch the active bank map with a final meta commit.
Preflight the combined entry cost of all changed profiles so
`ESP_ERR_NVS_NOT_ENOUGH_SPACE` on a portal re-save (seen on v0.5.0+PR#27)
cannot recur with the maximum 5×5 config.

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
| `cfg_meta` | `dash_meta_blob_t`: version, caps, refresh_min, network_count, panel_variant, max_partials, write_counter, per-slot bank map, crc32 | 24 B |
| `cfg_net{0..4}{a\|b}` | `dash_net_blob_t`: version, crc32, one `dash_wifi_profile_t` (profile + its 5 APIs + its quiet-hours fields) | ~1 440 B |

Each slot has two bank keys (`a`/`b`); the meta blob records which bank is
live per slot. A changed network is written to the slot's *other* bank, and
the meta write at the end of the save switches the new generation live
atomically (Codex review round 1, [MAJOR] #3: without this, a failed save
could leave a mix of old and new networks while the portal reports
"nothing applied").

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
3. **Erase orphans only**: bank keys the stored meta does not reference
   (leftovers of a previously failed save). Everything the old meta
   references stays untouched until step 6 commits. This cleanup runs before
   the capacity guard so blobs stranded by a failed attempt cannot block its
   retry.
4. **Preflight**: build and compare every candidate against its live bank,
   recording all slots that need a write. Use `nvs_get_stats()` and require
   `available_entries` to cover the combined NVS entry cost of every pending
   network blob plus the meta update and slack. `available_entries` excludes
   the reserved GC page, unlike `free_entries`. Fail before writing with
   `ESP_ERR_NVS_NOT_ENOUGH_SPACE` when the complete atomic save cannot fit.
5. **Per-network, sequentially**: write each changed/new candidate into its
   slot's OTHER bank, committing one key at a time. Unchanged slots keep
   their current bank and incur no flash write.
6. **Meta last — the atomic switch**: if anything changed, write `cfg_meta`
   (new bank map, `write_counter + 1`). Only this commit makes the new
   generation live; a failure or power cut before it leaves the old config
   fully intact, so a failed portal save genuinely means "nothing applied".
   A fully unchanged save writes nothing at all (today it rewrites 7.2 KB
   even for a no-op).
7. **Release the old generation** (best-effort, after the meta is live):
   erase switched banks, dropped slots, and the legacy `cfg_v2` key; any
   leftover is an orphan that step 3 of the next save reclaims.

A power cut during the meta `nvs_set_blob` itself is covered by NVS's own
item-level power-safety: the old meta entry is only invalidated after the
new one is fully written, so the device boots with either the complete old
or the complete new generation — never a torn one.

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
- The runtime preflight calculates the exact requirement for the current
  save: all changed alternate-bank blobs plus the meta update and slack.

Static asserts (storage.c):

- `sizeof(dash_net_blob_t) <= DASH_NET_BLOB_MAX_BYTES`,
  `sizeof(dash_meta_blob_t) <= 32`.
- Absolute worst-case occupancy fits:
  `2 × 5 × entries(net) + 2 × entries(meta) + MISC
   <= 5 pages × 126 entries`, where MISC = 32 entries reserved for
  `ap_pwd`, `wifi_cc`, namespace bookkeeping, and slack. This models every
  network slot double-banked until the atomic meta switch, plus old+new meta.

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
