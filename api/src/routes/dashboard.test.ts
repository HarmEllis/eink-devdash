import { test } from 'node:test'
import assert from 'node:assert/strict'

import { formatLocalIso, formatLocalUpdatedAt } from './dashboard.js'

// A fixed instant: 2026-06-03T21:14:09Z. In Europe/Amsterdam (UTC+2 in summer)
// that is local 23:14:09; in UTC it is 21:14:09.
const INSTANT = new Date('2026-06-03T21:14:09.000Z')

test('formatLocalIso renders an offset-less local wall-clock timestamp', () => {
  assert.equal(
    formatLocalIso(INSTANT, 'Europe/Amsterdam'),
    '2026-06-03T23:14:09',
  )
})

test('formatLocalIso honors the requested timezone', () => {
  assert.equal(formatLocalIso(INSTANT, 'UTC'), '2026-06-03T21:14:09')
})

test('formatLocalIso normalizes midnight to 00 (not 24)', () => {
  // 2026-06-03T22:00:00Z is local 00:00 in Amsterdam (UTC+2), rolling the date.
  const midnight = new Date('2026-06-03T22:00:00.000Z')
  assert.equal(
    formatLocalIso(midnight, 'Europe/Amsterdam'),
    '2026-06-04T00:00:00',
  )
})

test('formatLocalIso is parseable by the same HH:MM contract as updatedAtLocal', () => {
  // The firmware extracts HH:MM from index 11..15 of an ISO string; assert the
  // two fields agree so the display clock stays consistent with the parsed time.
  const iso = formatLocalIso(INSTANT, 'Europe/Amsterdam')
  const hhmm = formatLocalUpdatedAt(INSTANT, 'Europe/Amsterdam')
  assert.equal(iso.slice(11, 16), hhmm)
})
