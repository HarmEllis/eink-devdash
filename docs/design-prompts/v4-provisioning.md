# Design prompt — V4 provisioning screens

Prompt used at `claude.ai/design` to extend the existing V3 e-ink
dashboard handoff with three additional screens for the device
provisioning flow (e-ink setup prompt, mobile captive portal config
page, and flash-server post-flash instruction page).

The V3 handoff lives at
`.temp/design-extracted/design_handoff_eink_v3_dashboard/` and is the
canonical visual reference this prompt builds on. The output should
land alongside it as `design_handoff_eink_v4_provisioning/`.

Related: [ADR-0002](../decisions/0002-softap-portal-captive-qr.md).

---

Update the existing DevDash V3 e-ink design handoff with three new screens
for the device-provisioning flow. The current V3 covers only the operational
dashboard (GitHub issues / PRs / Claude+Codex usage bars). I need three
additional first-time-setup screens that share the same visual language
where applicable.

## What stays the same

For the e-ink screen (#1 below), reuse the V3 visual system exactly:
- Resolution: 296 × 128 px, landscape, 2 × 1-bit planes (BLACK + RED).
- Allowed pixels: white, black, red only — no greys, no anti-aliasing.
- Fonts: Silkscreen 8 px (labels/body), Pixelify Sans 16 px (big values).
  Both rendered without anti-aliasing.
- Red is expensive (refresh ~15–27 s with red vs ~3 s B/W-only). Use red
  only when essential to grab attention (e.g. an error state).
- Same outer 1-px border and 2-px edge margin as V3.
- Same header band shape (logo + identifier at left, status at right).

For the mobile HTML screens (#2, #3), I want a clean modern look that
**does not** match the pixel-art e-ink style — the phone screen should feel
like a normal web form, not a retro toy. A neutral system look is fine
(SF/Roboto system font stack, light theme, generous tap targets). Mobile
first; the captive portal popup on iOS and Android is the primary surface.

## Hard constraints for the mobile HTML (#2, #3)

These are non-negotiable because of how the screens are served:

- The phone has **no internet** while connected to the device's SoftAP.
  Everything must be self-contained: **inline CSS, system fonts only**, no
  CDN, no Google Fonts, no external images. Inline SVG for any iconography.
- The HTML is **server-rendered in C** on the ESP32 (no framework). Keep
  the markup simple — semantic HTML + class-driven styling. No build step.
- Inline JS is allowed but should be minimal — used only for client-side
  validation, show/hide toggles, and "clear secret" checkboxes. The form
  must remain usable with JS disabled.
- Total page weight target: under 30 KB rendered HTML/CSS combined.

## Screen 1 — E-ink provisioning prompt (296 × 128)

Shown when the device boots without WiFi credentials, or when the user
presses the BOOT button to re-enter setup mode. Replaces the current
"DEVDASH / OFFLINE" V3 placeholder for this state.

Content:
- Header row (reuse V3 header shape): logo glyph + "DEVDASH" on the left,
  the text "SETUP" on the right where the clock normally is. No clock —
  device is not online.
- Main area, left half: a square QR code, roughly 99 × 99 px, occupying
  the left ~110 px of the canvas. The QR encodes the WiFi join string
  (WIFI:T:WPA;S:devdash-XXXX;P:<password>;;). Use a 1 px white quiet zone
  around the QR.
- Main area, right half: three labelled lines in Silkscreen 8 px,
  left-aligned, with a small icon per line:
    - WiFi icon  +  SSID:  devdash-XXXX
    - Key icon   +  PASS:  <12-char password>
    - Globe icon +  URL:   192.168.4.1
- Footer (full width, at the bottom): one line of Silkscreen 8 px,
  centred — "Scan with phone camera"
- No red anywhere on this screen in the normal state.
- Error variant: if the SoftAP failed to start, replace the QR area with
  a red X glyph (reuse CrossSync style) and the text "SETUP FAILED" in red,
  same layout otherwise.

Provide the same pixel-exact layout, dividers, and icon definitions you
provided for V3.

## Screen 2 — Captive portal config page (mobile HTML)

Shown when the phone joins the SoftAP and the captive portal popup
opens. This is the main configuration UI for both first-time setup and
later edits (e.g. swapping API URL when moving the device between home
and office).

Sections (top to bottom on mobile):
1. Top bar: device name ("devdash-XXXX") and a small "connected to AP"
   indicator. No nav, no menu.
2. WiFi networks — up to 5 entries. Each entry shows:
   - An enable checkbox
   - SSID text input
   - Password input with placeholder "current password kept" when the
     entry has an existing saved password; a small "Clear password"
     checkbox that, when ticked, forces the password to be erased on save.
     For new entries (no saved password yet), the password is required.
   - A "Remove this network" link/button
   - An expandable sub-section: APIs for this network (see below)
   - "Add API to this network" button (max 5 per network)
   "Add another WiFi network" button below the list, disabled at 5 networks.
3. APIs per network — up to 5 per network. Each entry shows:
   - Enable checkbox
   - API URL text input (placeholder: http://192.168.1.50:3000)
   - Device token input with the same "current token kept" / "Clear token"
     semantics as WiFi password
4. Display section:
   - Refresh interval slider or number input, 3–60 minutes, default 5.
5. Sticky Save button at the bottom of the viewport (mobile pattern),
   plus an inline validation summary above it on errors.

Include states:
- Empty (fresh device, no networks configured) — show one empty WiFi
  card and one empty API card pre-rendered.
- Populated (existing config) — show 2 networks each with 1-2 APIs as
  the example.
- Validation error — show what an inline field error looks like (e.g.
  malformed URL).

Confirmation page after Save:
- Centred message: "Saved. Device restarting…" with a small spinner
  (CSS-only, no external animation libs).
- After ~3 seconds the page tells the user they can close the tab.

## Screen 3 — Flash-server post-flash instruction (desktop HTML)

Shown in the flash-server browser tab (port 8080) after the user
successfully flashes the firmware via ESP Web Tools. Replaces the
current Improv/config UI on that page.

Content:
- Big success indicator at the top: "Flashed successfully".
- One short paragraph: "Look at the device screen — it now shows a QR
  code. Scan it with your phone camera, then follow the popup to set
  up WiFi."
- A small illustration / mockup of the e-ink screen showing the QR (you
  can reuse the actual Screen 1 layout as a static illustration here).
- A collapsible "Troubleshooting" panel with two items:
  - "I don't see a QR on the screen" — short note about pressing BOOT.
  - "The popup didn't appear on my phone" — short note to manually
    visit http://192.168.4.1 after joining the AP.
- This page is loaded over the local LAN from a Node static server,
  so for screen 3 only, external assets (Google Fonts, etc.) ARE
  allowed — but please keep it self-contained anyway for offline use.

## Deliverables

For each screen, provide:
- A pixel-accurate or mobile-accurate HTML/React preview file like
  `v3_preview.html` (self-contained, opens in a browser).
- Coordinate tables / class structure documentation in a README per
  screen, same style as the V3 README.
- For Screen 1, the icon bitmaps as constants like V3.

Bundle as `design_handoff_eink_v4_provisioning/` alongside the
existing v3 dashboard handoff.
