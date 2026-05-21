# ADR-0003: Red-free black/white partial refresh mode for the tri-color e-paper

- **Status:** Proposed
- **Date:** 2026-05-21
- **Branch where proposed:** `main`
- **Relates to:** the SSD1680 display driver in
  `firmware/components/eink_weact29/` and the dashboard renderer in
  `firmware/main/display.c`.

## Context

The production display is a WeAct 2.9 inch black/white/red e-paper
module driven by an SSD1680-compatible controller. Red refreshes are
slow, taking roughly 15-20 seconds, but most dashboard frames are
expected to be black/white only. The dashboard should therefore update
volatile content such as the last update time, usage percentages, and
usage bars quickly when no red content is required.

The current firmware already has two conceptual refresh modes:

- `EINK_REFRESH_FULL_COLOR`, which writes both the black/white RAM and
  the red RAM.
- `EINK_REFRESH_BW_FAST`, which writes only the black/white RAM.

However, the driver currently sends the panel to deep sleep after each
refresh. On the next wake, `eink_refresh()` resets the controller and
forces `EINK_REFRESH_BW_FAST` back to `EINK_REFRESH_FULL_COLOR`. In
practice this prevents a sustained black/white partial-refresh episode.

The key question is whether a black/white/red panel can be operated as
if it were its black/white sibling while the visible frame contains no
red content.

## Decision

Implement and validate a **red-free black/white operating mode** for
normal dashboard updates.

When both the previous visible frame and the next rendered frame contain
no red pixels, the firmware may treat the panel as black/white only:

- Keep the red RAM untouched or explicitly in the white/no-red state.
- Write only black/white RAM for fast updates.
- Use partial windows for volatile dashboard regions, especially the
  header time/status, percentage labels, and usage bars.
- Periodically perform a full clean/full redraw after a bounded number
  of fast updates to control ghosting.

When red is required, switch to the full-color path. When leaving a
red-bearing frame, perform one full-color refresh to physically clear
the red pigment before returning to the red-free black/white mode.

This is not a decision to implement partial color refresh. Red remains
full-color only.

## Evidence

This approach is supported by multiple independent references:

- Waveshare's 2.9 inch e-Paper Module (B) V4 documentation says the V4
  demo adds fast refreshing and black/white partial refreshing for a
  black/white/red display.
- Waveshare's V4 driver implements `Partial(...)` by writing only the
  black/white RAM command (`0x24`) and then triggering the partial
  update; it does not write red RAM for that path.
- A GxEPD2 user tested a 2.9 inch black/white/red Waveshare display
  with a black/white driver definition and reported that partial
  refresh worked, was much faster, and used only black and white.
- GxEPD2's changelog records "fast b/w refresh for capable 3-color
  displays" and reports that up to 100 black/white fast refreshes were
  possible on some 3-color panels, with a slightly reddish background
  as a known artifact risk.
- The SSD1680 datasheet describes separate mono black/white and mono
  red RAM planes and support for display partial update at the
  controller level.

There is also conflicting vendor guidance: WeAct and some third-party
driver matrices state that the black/white/red module does not support
partial or fast refresh, while the black/white module does. Treat this
ADR as an implementation direction that must be confirmed on the actual
hardware.

## Consequences

- The display driver should expose an explicit red-free black/white
  mode instead of relying on `EINK_REFRESH_BW_FAST` behind the
  tri-color initialization path.
- The display state machine must track whether the currently visible
  frame is red-free and whether the controller has a valid base frame
  for differential or partial black/white refresh.
- Deep sleep policy needs to change for fast update episodes. Either
  keep enough controller state alive between short updates or add a
  black/white-compatible wake/init path that preserves the assumptions
  required by partial refresh.
- Partial window support should be added to the low-level driver so the
  renderer can update only volatile regions instead of rewriting the
  full black/white frame every time.
- The renderer must keep full-frame black/white buffers in RTC memory or
  another retained store so unchanged regions and base-frame assumptions
  remain coherent across deep-sleep wakeups.
- Full-color refresh remains mandatory when entering red content and
  when clearing red content.
- A full clean/full redraw should run after a conservative number of
  black/white partial updates. Start with the existing limit of 10 and
  adjust only after hardware testing.

## Validation Plan

Validate on the physical WeAct 2.9 inch black/white/red panel:

1. Start from a full-color all-white or red-free dashboard base frame.
2. Update only the time area with a partial black/white refresh.
3. Update percentage labels and bars with partial black/white refreshes.
4. Repeat at least 10 cycles and inspect for ghosting, reddish tint, and
   wrong-region artifacts.
5. Trigger a red-bearing frame and confirm it uses full-color refresh.
6. Return to a red-free frame with one full-color clear/redraw, then
   confirm black/white partial updates work again.
7. Test across the intended display orientation because existing
   reports mention rotation/window limitations on some panel variants.

## References

- [Waveshare 2.9 inch e-Paper Module (B) Manual](https://www.waveshare.com/wiki/2.9inch_e-Paper_Module_%28B%29_Manual)
- [Waveshare 2.9B V4 Arduino driver](https://raw.githubusercontent.com/waveshareteam/e-Paper/master/Arduino/epd2in9b_V4/epd2in9b_V4.cpp)
- [GxEPD2 project and changelog](https://github.com/ZinggJM/GxEPD2)
- [Arduino forum: partial update on 2.9 inch B/W/R e-paper](https://forum.arduino.cc/t/problem-gxepd2-partial-update-on-2-9-b-w-r-e-paper/956398)
- [SSD1680 datasheet](https://files.seeedstudio.com/wiki/Other_Display/213-epaper/IC%20Driver%20SSD1680%20Datasheet.pdf)
- [WeAct product note with conflicting support claim](https://cntronic.com/weact-2-9-2-13-2-9-2-13-inch-epaper-module-e-paper-e-ink-eink-display-screen-spi-black-white-black-white-red-126907)
- [Unofficial WeAct Rust driver support matrix](https://docs.rs/weact-studio-epd/latest/weact_studio_epd/)
