import { test } from 'node:test'
import assert from 'node:assert/strict'

import { inferWindowLabel, usageFromRateLimits } from './codex.service.js'

// For inferWindowLabel tests an arbitrary fixed value works because it only
// computes (resetsAt - nowSeconds) deltas.
const NOW_FIXED = 1_000_000 // epoch seconds, fixed for label tests
const DAY = 24 * 60 * 60

// For usageFromRateLimits tests we need real future timestamps because
// windowFromRateLimit zero-clamps usedPercent when resetsAt <= Date.now()/1000.
const NOW_REAL = Date.now() / 1000

// --- inferWindowLabel ---------------------------------------------------------

test('inferWindowLabel: limit_id "week" -> "7d"', () => {
  assert.equal(inferWindowLabel(null, 'week', '5h', NOW_FIXED), '7d')
})

test('inferWindowLabel: limit_id "weekly" -> "7d"', () => {
  assert.equal(inferWindowLabel(null, 'weekly', '5h', NOW_FIXED), '7d')
})

test('inferWindowLabel: limit_id "7d" -> "7d"', () => {
  assert.equal(inferWindowLabel(null, '7d', '5h', NOW_FIXED), '7d')
})

test('inferWindowLabel: limit_id "5h" -> "5h"', () => {
  assert.equal(inferWindowLabel(null, '5h', '7d', NOW_FIXED), '5h')
})

test('inferWindowLabel: limit_id "hour" -> "5h"', () => {
  assert.equal(inferWindowLabel(null, 'hour', '7d', NOW_FIXED), '5h')
})

test('inferWindowLabel: limit_id "primary" -> "5h"', () => {
  assert.equal(inferWindowLabel(null, 'primary', '7d', NOW_FIXED), '5h')
})

test('inferWindowLabel: limit_id is case-insensitive', () => {
  assert.equal(inferWindowLabel(null, 'WEEKLY', '5h', NOW_FIXED), '7d')
  assert.equal(inferWindowLabel(null, 'Week', '5h', NOW_FIXED), '7d')
})

test('inferWindowLabel: resetsAt > 24 h away -> "7d"', () => {
  // Matches the actual Codex response from the bug report (554861 s = 6.4 d)
  const resetsAt = NOW_FIXED + 6.4 * DAY
  const window = { resets_at: resetsAt }
  assert.equal(inferWindowLabel(window, undefined, '5h', NOW_FIXED), '7d')
})

test('inferWindowLabel: resetsAt exactly 2 days away -> "7d"', () => {
  const window = { resets_at: NOW_FIXED + 2 * DAY }
  assert.equal(inferWindowLabel(window, undefined, '7d', NOW_FIXED), '7d')
})

test("inferWindowLabel: resetsAt 3 hours away, default '5h' -> stays '5h'", () => {
  // reset <= 24 h: heuristic does not fire; fallback to defaultLabel '5h'
  const window = { resets_at: NOW_FIXED + 3 * 60 * 60 }
  assert.equal(inferWindowLabel(window, undefined, '5h', NOW_FIXED), '5h')
})

test("inferWindowLabel: resetsAt 3 hours away, default '7d' -> stays '7d' (no downgrade)", () => {
  // P2-1: a weekly window in its final day must NOT be downgraded to '5h'.
  // The heuristic only upgrades (5h->7d when reset > 24h); when it does not
  // fire the defaultLabel is returned unchanged, preserving '7d'.
  const window = { resets_at: NOW_FIXED + 3 * 60 * 60 }
  assert.equal(inferWindowLabel(window, undefined, '7d', NOW_FIXED), '7d')
})

test('inferWindowLabel: resetsAt exactly 24 h away -> defaultLabel (boundary, not > 24 h)', () => {
  const window = { resets_at: NOW_FIXED + DAY }
  assert.equal(inferWindowLabel(window, undefined, '5h', NOW_FIXED), '5h')
})

test('inferWindowLabel: accepts camelCase resetsAt field', () => {
  const window = { resetsAt: NOW_FIXED + 2 * DAY }
  assert.equal(inferWindowLabel(window, undefined, '5h', NOW_FIXED), '7d')
})

test('inferWindowLabel: no limit_id, no resetsAt -> uses defaultLabel', () => {
  assert.equal(inferWindowLabel(null, undefined, '7d', NOW_FIXED), '7d')
  assert.equal(inferWindowLabel(undefined, undefined, '5h', NOW_FIXED), '5h')
  assert.equal(inferWindowLabel({ resets_at: undefined }, null, '7d', NOW_FIXED), '7d')
})

test('inferWindowLabel: unknown limit_id falls through to reset heuristic', () => {
  const window = { resets_at: NOW_FIXED + 3 * DAY }
  assert.equal(inferWindowLabel(window, 'unknown_id', '5h', NOW_FIXED), '7d')
})

// --- usageFromRateLimits slot assignment (P2-2) --------------------------------
// Use NOW_REAL (current wall-clock seconds) so resetsAt is in the future and
// windowFromRateLimit does not zero-clamp usedPercent.

test('usageFromRateLimits: weekly-only primary is moved into long slot', () => {
  // Replicates the real-world case: Codex has only a weekly limit and the
  // app-server puts it in primary with resetsAt ~6.4 days away.  The weekly
  // window must end up in usage.long so downstream enrichment (daily-slice
  // cutoff, tick) applies to it; leaving it in short would skip those features.
  const weeklyReset = NOW_REAL + 6.4 * DAY
  const usage = usageFromRateLimits({
    primary: { used_percent: 11, resets_at: weeklyReset },
    secondary: null,
  })
  assert.equal(usage.long.label, '7d', 'inferred weekly primary must land in long slot')
  assert.equal(usage.short.label, '5h', 'short slot keeps the fallback label')
  assert.ok(usage.long.resetInSeconds > 0, 'long slot carries the reset countdown')
  assert.equal(usage.short.usedPercent, 0, 'short slot is empty when source is null')
})

test('usageFromRateLimits: normal two-window layout keeps primary in short slot', () => {
  // When both primary (short, 4h reset) and secondary (long, 5d reset) are
  // present the original slot order should be preserved unchanged.
  const shortReset = NOW_REAL + 4 * 60 * 60 // 4 h
  const longReset  = NOW_REAL + 5 * DAY
  const usage = usageFromRateLimits({
    primary:   { used_percent: 37, resets_at: shortReset },
    secondary: { used_percent: 20, resets_at: longReset  },
  })
  assert.equal(usage.short.label, '5h')
  assert.equal(usage.long.label,  '7d')
  assert.ok(usage.short.usedPercent > 0)
  assert.ok(usage.long.usedPercent  > 0)
})

test('usageFromRateLimits: secondary window near reset keeps 7d label in long slot', () => {
  // P2-1 + P2-2 combined: the secondary window has defaultLabel '7d' and resets
  // in < 24 h.  The heuristic does not fire (reset not > 24 h), so the label
  // falls back to '7d' - no downgrade.  It stays in long because it is secondary.
  const nearReset = NOW_REAL + 3 * 60 * 60
  const usage = usageFromRateLimits({
    primary:   null,
    secondary: { used_percent: 90, resets_at: nearReset },
  })
  assert.equal(usage.long.label, '7d', 'secondary near reset keeps 7d label in long slot')
  assert.ok(usage.long.usedPercent > 0)
})
