# Plan: Keep BW Partial Refreshes Eligible During Alert States

## Summary

Fix the dashboard refresh policy by separating two concepts:

- The UI wants to draw an alert highlight.
- The physical panel refresh needs red pigment.

On a BW panel, alert highlights already collapse to black/white through
`lpix()`, so they must not set the physical `need_red` policy flag. On a BWR
panel, the current behavior remains correct: alert frames need red and therefore
must use a full-color refresh.

## Key Changes

- Update the dashboard refresh policy in `firmware/main/display.c`.
- Compute an abstract alert flag for the existing alert conditions:
  Dependabot alerts, GitHub auth errors, offline/stale dashboard frames, Claude
  or Codex usage above 80%, Codex reached-limit state, and Claude auth errors.
- Set the physical `need_red` flag only when the effective panel variant is
  `EINK_PANEL_WEACT_29_BWR` and the abstract alert flag is true.
- Keep the existing drawing calls intact; `lpix()` already maps `use_red`
  operations to black/white pixels on BW panels.
- Keep `s_last_red_state` behavior unchanged. It is already panel-aware and is
  stored as false on BW panels after a full refresh.
- Improve the dashboard decision log enough to show both the physical
  `need_red` result and the abstract alert state.

## Test Plan

- Add a small helper or otherwise make the red-policy logic testable without
  requiring the display driver.
- Cover at least this matrix:
  - BW and BWR panel variants.
  - Claude `authError` true and false.
  - At least one non-Claude alert condition, such as usage over 80% or Codex
    reached-limit state.
- Acceptance scenarios:
  - BW plus Claude `authError=true` yields physical `need_red=false`, so the BW
    partial path remains eligible when the other partial preconditions are met.
  - BW plus Codex reached limit, high usage, Dependabot alert, or GitHub auth
    error also yields physical `need_red=false`.
  - BWR plus any of those alert conditions still yields physical
    `need_red=true`.
  - Existing full-refresh reasons still win on BW: first render, layout flip,
    variant change, 24-hour render cap, per-region partial cap, and previous
    non-content frames.
- Verify with the firmware build inside the devcontainer:

```bash
docker exec -u node optimistic_hermann bash -c "source /etc/profile.d/esp-idf.sh && cd /workspaces/eink-devdash/firmware && idf.py build"
```

## Assumptions

- This plan only changes the normal dashboard partial-refresh policy.
- Offline, OTA, provisioning, boot, and connecting surfaces remain
  full-refresh-only by design.
- BW alert highlights rendered as black/white are acceptable because that is
  already how the framebuffer path behaves.
- BWR behavior must not change: alert dashboard frames remain red-bearing and
  full-color refreshed.
