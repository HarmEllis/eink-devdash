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
