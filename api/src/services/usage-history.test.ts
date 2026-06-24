import { test, beforeEach } from 'node:test'
import assert from 'node:assert/strict'

import {
  recordAndComputeRecent,
  resetUsageHistory,
  usageHistoryKey,
} from './usage-history.js'

const HOUR = 60 * 60 * 1000
const MIN = 60 * 1000

beforeEach(() => resetUsageHistory())

test('first reading has no baseline, so recent is 0', () => {
  const recent = recordAndComputeRecent('a:long', 40, null, 1_000 * HOUR)
  assert.equal(recent, 0)
})

test('during warm-up it diffs against the oldest sample we have', () => {
  const t0 = 1_000 * HOUR
  const reset = t0 + 30 * HOUR
  recordAndComputeRecent('a:long', 40, reset, t0) // oldest sample
  recordAndComputeRecent('a:long', 45, reset, t0 + 30 * MIN)
  // No sample is >=1h old yet, so the baseline is the oldest (t0, used 40).
  const recent = recordAndComputeRecent('a:long', 50, reset, t0 + 50 * MIN)
  assert.equal(recent, 10) // 50 - 40
})

test('recent is the diff against the reading ~1h ago', () => {
  const t0 = 1_000 * HOUR
  const reset = t0 + 30 * HOUR
  recordAndComputeRecent('a:long', 40, reset, t0) // baseline
  recordAndComputeRecent('a:long', 48, reset, t0 + 30 * MIN)
  const recent = recordAndComputeRecent('a:long', 55, reset, t0 + 61 * MIN)
  assert.equal(recent, 15) // 55 - 40
})

test('a decreasing rolling window never yields a negative recent', () => {
  const t0 = 1_000 * HOUR
  const reset = t0 + 30 * HOUR
  recordAndComputeRecent('a:short', 60, reset, t0)
  const recent = recordAndComputeRecent('a:short', 50, reset, t0 + 61 * MIN)
  assert.equal(recent, 0)
})

test('window-reset guard: when the reset instant jumps, all current usage is recent', () => {
  const t0 = 1_000 * HOUR
  const resetOld = t0 + 20 * MIN // about to reset
  recordAndComputeRecent('a:short', 95, resetOld, t0) // baseline, old window
  // An hour later the window has reset: usedPercent dropped and resetAt jumped
  // forward by ~5h. A naive diff (5 - 95) would be clamped to 0 and hide it.
  const resetNew = t0 + 61 * MIN + 5 * HOUR
  const recent = recordAndComputeRecent('a:short', 5, resetNew, t0 + 61 * MIN)
  assert.equal(recent, 5)
})

test('a null resetAt (resetInSeconds 0/absent) does not trip the reset guard', () => {
  const t0 = 1_000 * HOUR
  const reset = t0 + 30 * HOUR
  recordAndComputeRecent('a:long', 40, reset, t0) // baseline has a real resetAt
  // Provider momentarily drops the countdown -> resetAt null. Must keep diffing,
  // not flag the whole usage as recent.
  const recent = recordAndComputeRecent('a:long', 44, null, t0 + 61 * MIN)
  assert.equal(recent, 4)
})

test('keys are isolated per service+window', () => {
  const t0 = 1_000 * HOUR
  const reset = t0 + 30 * HOUR
  const k1 = usageHistoryKey('claude', 'weekly')
  const k2 = usageHistoryKey('codex', 'long')
  recordAndComputeRecent(k1, 10, reset, t0)
  recordAndComputeRecent(k2, 80, reset, t0)
  assert.equal(recordAndComputeRecent(k1, 12, reset, t0 + 61 * MIN), 2)
  assert.equal(recordAndComputeRecent(k2, 85, reset, t0 + 61 * MIN), 5)
})

// --- Day cutoff (the "today" slice on the 7d window) ---------------------

test('day cutoff: recent is the diff against the sample before local midnight', () => {
  const midnight = 1_000 * 24 * HOUR // pretend this is local midnight
  const reset = midnight + 5 * 24 * HOUR
  // A reading just before midnight is the baseline for the whole day.
  recordAndComputeRecent('a:weekly', 30, reset, midnight - 5 * MIN, midnight)
  // Later that day, polled with the same day cutoff.
  recordAndComputeRecent('a:weekly', 42, reset, midnight + 3 * HOUR, midnight)
  const recent = recordAndComputeRecent('a:weekly', 55, reset, midnight + 9 * HOUR, midnight)
  assert.equal(recent, 25) // 55 - 30, regardless of how long ago midnight was
})

test('day cutoff warm-up: a same-day restart diffs against the oldest sample', () => {
  const midnight = 1_000 * 24 * HOUR
  const reset = midnight + 5 * 24 * HOUR
  // First sample only appears at 10:00 (process started after midnight), so
  // there is nothing before the cutoff yet -> diff against the oldest we have.
  recordAndComputeRecent('a:weekly', 40, reset, midnight + 10 * HOUR, midnight)
  const recent = recordAndComputeRecent('a:weekly', 47, reset, midnight + 12 * HOUR, midnight)
  assert.equal(recent, 7) // 47 - 40
})

test('day cutoff: the reset guard still flags a window that reset during the day', () => {
  const midnight = 1_000 * 24 * HOUR
  const resetOld = midnight + 10 * MIN
  recordAndComputeRecent('a:weekly', 90, resetOld, midnight - 5 * MIN, midnight)
  const resetNew = midnight + 5 * HOUR
  const recent = recordAndComputeRecent('a:weekly', 8, resetNew, midnight + 6 * HOUR, midnight)
  assert.equal(recent, 8) // whole current window counts as recent after a reset
})

test('coalescing keeps the hour baseline correct under fast (sub-minute) polling', () => {
  const t0 = 1_000 * HOUR
  const reset = t0 + 30 * HOUR
  // Poll every 10s for 70 minutes along a 0..70 ramp. Coalescing downsamples to
  // ~1/min but must still retain a sample ~1h old, so the last-hour slice is the
  // real ramp (not 0, which is what a perpetual-bump bug would yield).
  for (let t = t0; t <= t0 + 70 * MIN; t += 10 * 1000) {
    const used = ((t - t0) / (70 * MIN)) * 70
    recordAndComputeRecent('a:short', used, reset, t)
  }
  const recent = recordAndComputeRecent('a:short', 70, reset, t0 + 70 * MIN)
  // Baseline sits ~1h ago (the ~10-minute mark, used ~10), within ~1min skew.
  assert.ok(recent > 55 && recent < 65, `expected ~60, got ${recent}`)
})

test('day cutoff stays bounded and keeps the pre-midnight baseline under fast polling', () => {
  const midnight = 1_000 * 24 * HOUR
  const reset = midnight + 5 * 24 * HOUR
  const key = 'a:weekly'
  // The only sample at/under the cutoff: a low reading just before midnight.
  recordAndComputeRecent(key, 20, reset, midnight - MIN, midnight)
  // Then poll every 10s for a full 24h day (8640 readings) at a high plateau,
  // all strictly after midnight. Without coalescing this would blow past
  // MAX_SAMPLES and evict the pre-midnight baseline; the diff would then be
  // taken against the plateau (95 - 80 = 15) instead of the baseline.
  for (let t = midnight + 10 * 1000; t < midnight + 24 * HOUR; t += 10 * 1000) {
    recordAndComputeRecent(key, 80, reset, t, midnight)
  }
  const recent = recordAndComputeRecent(key, 95, reset, midnight + 24 * HOUR, midnight)
  // Diff is against the pre-midnight baseline (20), proving it survived the day.
  assert.equal(recent, 75) // 95 - 20
})
