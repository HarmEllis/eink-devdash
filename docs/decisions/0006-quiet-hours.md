# ADR-0006: Per-network quiet hours

- **Status:** Accepted
- **Date:** 2026-06-03

## Context

The device wakes on a timer every `refresh_min` minutes, connects to WiFi,
fetches the dashboard, repaints the e-paper, and deep-sleeps again. For a
desk dashboard that nobody looks at overnight, every one of those wakes still
spends the most expensive part of the budget — radio on, an HTTPS round trip,
and a panel refresh — for a screen no one reads.

We want a per-WiFi-network "quiet hours" window (e.g. 23:00–06:00) during which
the device skips the whole cycle and just sleeps. Per network because the same
device may sit on a home network (quiet overnight) and, when carried elsewhere,
on a network where a different or no window applies.

Two hard problems:

1. **Time source.** The window is wall-clock local time, but the firmware runs
   no SNTP and the RTC starts unset on every cold boot.
2. **Which network's window applies before WiFi is even on.** Evaluating a
   per-network window normally requires knowing which network is reachable,
   which requires turning the radio on — exactly what we want to avoid.

## Decision

### Time source: derive from the API, not SNTP

The API already returns the dashboard's local time. We add a full local
wall-clock field `updatedAtLocalIso` (`YYYY-MM-DDTHH:MM:SS`, offset-less) next
to the existing `updatedAtLocal` (HH:MM, kept for display). After each
successful fetch the firmware parses it and sets the RTC system clock
(`timekeep_set_from_iso`). The ESP32-S3 RTC keeps running across deep-sleep
timer wakes, so subsequent wakes evaluate the window without any network
activity. The clock is lost on cold boot / power loss, in which case the
window is simply not applied until the next successful fetch re-acquires it.

We treat the offset-less timestamp as UTC internally so `gmtime()` math is
offset- and DST-free — the server already encodes the local wall time the user
schedules against. This avoids a timezone/DST setting on the device and the
need for a reachable NTP server. Quiet hours are a coarse scheduling hint, not
a precision clock feature, so RTC drift across a multi-hour window is
acceptable.

### Which network: the last-success network

On a plain timer wake we evaluate the window of `last_success_network_idx` —
the network the device is parked on — guarded by: the index is in range, that
network is still enabled with a non-empty SSID, quiet hours is enabled for it,
and the RTC clock is valid. Cold boot, provisioning long-press, and a
short-press EXT0 force-refresh all bypass quiet hours and fetch normally.

### Chunked sleep, not one long sleep

`enter_deep_sleep` is capped at 60 minutes and `uint8_t`. Instead of one long
sleep to the window end, the device sleeps in chunks of up to 60 minutes and
re-evaluates the window on each wake (radio stays off, so this is cheap). This
sidesteps the cap, self-corrects against RTC drift, and leaves the window
within at most one chunk. A 60-second floor prevents a tight wake loop near the
boundary.

### Storage: trailing parallel arrays (schema v5)

Quiet settings are stored as trailing parallel arrays indexed by network slot
(`quiet_enabled[]`, `quiet_start_min[]`, `quiet_end_min[]`), appended after the
existing config rather than embedded in `dash_wifi_profile_t`. This keeps the
`networks[]` byte offset identical to v2/v3/v4, so the established migration
invariants hold and v4→v5 is a trivial zero-seed (quiet disabled by default).
Unlike the v3→v4 `max_partials` field, the arrays do not fit in tail padding,
so `sizeof(v5) > sizeof(v4)`: the load path distinguishes v5 from v3/v4 by blob
length, then v3 from v4 by the `version` field. A window with `start == end` is
normalized to disabled.

### Display: sleeping overlay

On entering the window the device repaints once: the last dashboard with a moon
+ last-sync time in the header (no "+Nm" next-refresh hint) and a black footer
bar `[moon] SLEEPING [dot] WAKES HH:MM`. The repaint is gated by an RTC flag so
the chunked re-evaluations do not repaint. The footer sits below the per-region
partial-refresh rectangles, so the first dashboard render after the window
forces a full refresh (another RTC flag) to clear it cleanly.

## Consequences

- A device with no recent fetch (cold boot during the window) does one normal
  refresh before honoring quiet hours. Acceptable: nobody is watching.
- The window follows the API server's local time, not the device's physical
  location. For this single-owner desk dashboard that is the intended behavior.
- The feature is inert until configured; existing devices migrate with quiet
  hours disabled.

## Extension: always-connected operation

As of 2026-06-13, the portal can enable a device-wide always-connected mode.
Normal refresh intervals then use an in-process FreeRTOS wait with WiFi
minimum-modem power save instead of deep sleep. The refresh cycle reuses a
valid association and falls back to the existing scan/roam path after a
disconnect.

Each network also has a quiet-hours deep-sleep choice. It defaults to enabled,
preserving the original behavior. When disabled together with always-connected
mode, quiet hours pause API and display updates while the device stays awake
and maintains its WiFi association.

The two opt-ins reuse bytes that were zero in existing v6 data: the final
reserved meta byte for the device-wide mode and a former alignment byte in
each network profile for the quiet-hours override. Blob sizes, CRC coverage,
the schema version, and the NVS partition layout remain unchanged. Static
offset assertions enforce that compatibility.
