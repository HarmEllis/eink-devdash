# Plan: reorder API endpoints in the captive portal

Date: 2026-06-09
Status: final, revision 3 (rev 1 and rev 2 reviewed by Codex, both
CHANGES_REQUESTED; this revision resolves all findings — maintainer approved
proceeding to implementation after this round)

## Goal

Let the user change the priority order of API endpoints within a WiFi
network in the captive portal with up/down arrow buttons. The configured
order is the **clean-start and fallback order**: on a wake without a usable
last-success hint, `api_client.c` tries `net->apis[idx]` in array order
(api_client.c:480-521). It is deliberately NOT authoritative on every wake —
see the constraint below.

Constraint from the maintainer: secrets must stay attached to their entry.
API device tokens (and WiFi passwords) are never sent back to the browser
for existing entries — the form posts an empty token field and the firmware
re-attaches the stored secret — so reordering must not break that
re-attachment.

## Scope decision: API rows only, no WiFi-network reordering

Rev 1 also proposed reordering WiFi network cards. Codex review and the
maintainer independently flagged that WiFi slot order is **not** the connect
priority: `wifi_roam_connect()` sorts visible candidates by descending RSSI
and uses the slot index only as a tie-breaker (wifi_roam.c:268-281, 329).
Reordering WiFi cards would therefore be a placebo. The maintainer chose to
keep RSSI-based selection and skip WiFi reordering entirely.

Follow-up from the same finding: the WiFi block hint currently says "the
device tries enabled networks in order, top first" (wifi_prov.c:917), which
is wrong today. This plan fixes that text to describe the actual behaviour
(strongest visible signal first).

## Constraint: last-success preference stays as-is

Maintainer requirement: the existing "remember which API worked and prefer
it after deep sleep" behaviour must not change. The configured order is only
the order for a clean start (cold boot, post-save restart, or when the hint
is unset/out of range).

This holds without code changes: `main.c` passes
`prefer_last_success_api=true` on timer/EXT0 wakes (main.c:337-355), and
that branch of `api_client_fetch_with_failover()` retries the last working
API before falling back to the others (api_client.c:471-478, 534-593); this
plan does not touch `api_client` or `main.c`. After a portal save the hints
are reset to -1 (existing behaviour: every save rebuilds from
`storage_cfg_v2_defaults()`, which sets both hints to -1, and
`storage_save_v2()` syncs the normalized hints into RTC,
storage.c:557-564), so a reorder takes effect as the clean-start order on
the post-save restart, while ordinary deep-sleep wakes keep preferring the
last working API.

Accepted consequence (Codex rev-2 MAJOR 1, resolved by documenting, per the
maintainer's explicit choice): once a lower-priority API succeeds, it stays
preferred on later wakes even after a higher-priority one recovers, until
it fails once or a portal save clears the hint. The portal hint text must
communicate this honestly (see §4) rather than claiming strict top-first
behaviour on every wake. Verification gains a check for this.

## Why the firmware already supports API reordering

`apply_form_to_cfg()` rebuilds each network's API list in **form slot
order** (k = 0..4) and re-attaches secrets by **id**, not by position: each
API row carries a hidden `wN_aK_i`, the old API is found by id within the
id-matched old network (wifi_prov.c:1166-1171), and its stored token is
inherited when the form posts none (wifi_prov.c:1183-1186). So if the
browser swaps two rows **and renames every form field to match the new slot
indices**, the firmware persists the new order with each token glued to its
entry. No storage-format, parser, or save-flow change is needed.

## Approach: client-side reorder, server changes limited to HTML/CSS/JS

All changes live in the static strings of `wifi_prov.c` (`V4_STYLE`,
`V4_JS`, `render_api`) plus two hint texts.

### 1. Render ↑/↓ buttons on API rows

- `render_api()`: add two small `type="button"` buttons (`data-move="up"` /
  `data-move="down"`) in `.api-row`, so they never submit the form.
- Buttons carry the `hidden` attribute in the server-rendered HTML; `V4_JS`
  unhides them on `DOMContentLoaded`. Reorder is JS-only; the no-JS portal
  stays exactly as functional as today.
- The API block hint gains a line describing the real semantics (see §4).

### 2. Move semantics: skip disabled rows (Codex MAJOR 2)

`apply_form_to_cfg()` skips disabled rows and compacts enabled APIs into
consecutive slots (wifi_prov.c:1161-1190), so only the **relative order of
enabled rows** persists; disabled rows are not saved at all. A naive
adjacent-sibling swap across a disabled row would look successful but
persist nothing. Therefore:

- arrows only act on **enabled source rows**: both move buttons of a row
  are `disabled` whenever that row's own `.api-on` checkbox is unchecked,
  and the click handler additionally returns early for a disabled source
  row (Codex rev-2 MAJOR 2 — moving a disabled row would be another
  successful-looking no-op, since disabled rows are discarded on save);
- an arrow click swaps the row with its **nearest enabled sibling** in that
  direction (enabled = its `.api-on` checkbox is checked at click time);
- if there is no enabled sibling in that direction, the click is a no-op;
  the handler keeps all buttons' `disabled` state in sync (on load, on
  toggle change, and after each move) so dead-end arrows render greyed out;
- disabled rows keep their position visually but are irrelevant to the
  persisted order — consistent with today's save semantics.

### 3. Swap + re-index logic in `V4_JS`

A delegated click handler on `[data-move]`:

1. Find the enclosing `li[data-api]` and its nearest enabled sibling in the
   move direction; none → return.
2. Swap DOM positions with `insertBefore`.
3. Re-index **all** API rows of that network card from their new DOM order
   (re-indexing everything is simpler and safer than renaming the two
   swapped rows, and avoids temp-name collisions): for the row at position
   k, rewrite descendant `name`s matching `/^w\d+_a\d+_/` to `w{n}_a{k}_`
   (n = the card's `data-net`, unchanged), plus `data-api="{n}-{k}"`,
   `data-target` on the cleartok checkbox, the `aria-label`, and the
   `.order` badge text (`k+1`).
4. Keep focus on the clicked button so keyboard users can press repeatedly,
   and refresh the buttons' disabled states.

Estimated JS addition: ~30 lines (~1 KB). The portal page is served from
flash strings; well within budget.

### 4. Hint-text fixes

- WiFi block hint (wifi_prov.c:917): replace "the device tries enabled
  networks in order, top first" with text describing RSSI-based selection,
  e.g. "the device connects to the strongest visible network".
- API section: describe the sticky-last-success semantics honestly, e.g.
  "Enabled APIs are tried top-first; once one works, the device keeps
  using it until it fails." (short form of the constraint above — no claim
  of strict top-first on every wake).

## Dropped from rev 1

- **WiFi-network reordering** — see scope decision above.
- **RTC last-success hint reset on save** — redundant: `apply_form_to_cfg()`
  starts from `storage_cfg_v2_defaults()`, which sets both hints to -1
  (storage.c:301-302), and `storage_save_v2()` syncs the normalized hints
  into RTC (storage.c:557-564), so every portal save already clears them.

## Testing (Codex SUGGESTION, partially deferred)

Codex suggested an automated DOM test plus a host-side reordered-form test.
Assessment:

- `apply_form_to_cfg()` is `static` in `wifi_prov.c` and entangled with
  `esp_http_server`/NVS types; extracting it into a host-testable unit is a
  refactor out of proportion to this change, and that code path is
  unchanged here. Deferred.
- The new code is ~30 lines of vanilla JS embedded in a C string; the repo
  has no JS test harness for firmware HTML. A repeatable middle ground that
  needs no new tooling: a small throwaway script (curl the rendered page
  from the device or paste it into a browser, run the move handler, and
  diff the serialized form names against the expected `wN_aK_*` layout)
  executed during manual verification. Not added to CI. If the maintainer
  wants the full harness instead, that is a separate decision.

## Touched files

- `firmware/main/wifi_prov.c` — `V4_STYLE` (button styling), `V4_JS`
  (unhide + swap/re-index + button-state logic), `render_api()` (emit
  buttons), two hint texts.

## Verification

1. `idf.py build` in the devcontainer (portal strings grow ~1.5 KB).
2. Manual portal test (on hardware, or by saving the rendered HTML and
   exercising the JS in a desktop browser):
   - move a saved API row with a stored token up/down past another
     **enabled** row, save **without retyping the token**, reopen the
     portal → order changed, placeholder still says "current token kept",
     and the device still fetches (token preserved and attached to the
     right URL);
   - quiet hours and WiFi password of the parent network unaffected;
   - arrows skip a disabled row in between: the two enabled rows swap
     places and the disabled row stays in its original visual slot;
   - a disabled row's own arrows are greyed and no-op (source-row guard);
   - top row's up / bottom enabled row's down arrows are greyed and no-op;
   - toggling a row's enable checkbox updates arrow disabled states;
   - sticky last-success unchanged: after a normal deep-sleep wake the
     device still retries the last working API first (serial log "API
     failover start: last successful profile"); after a reorder + save it
     starts from the new top entry ("first configured profile");
   - cleartok checkbox still disables the right input after a reorder
     (`data-target` renamed correctly);
   - order badges and API counts consistent after reorder;
   - form posts with swapped `wN_aK_*` names: verify via devtools that ids
     travel with their rows;
   - no-JS (disable JavaScript): portal renders and saves as today, no
     reorder buttons visible.
