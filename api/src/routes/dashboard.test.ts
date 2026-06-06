import { test } from 'node:test'
import assert from 'node:assert/strict'

import { buildDashboardPayload, formatLocalIso, formatLocalUpdatedAt, resolveTimeZone } from './dashboard.js'

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

test('resolveTimeZone falls back when a candidate is empty, blank, or invalid', () => {
  // docker-compose passes DASHBOARD_TIME_ZONE="" when the var is unset, which
  // `??` would not skip; the empty string must not reach Intl.
  assert.equal(resolveTimeZone(''), 'Europe/Amsterdam')
  assert.equal(resolveTimeZone('   '), 'Europe/Amsterdam')
  assert.equal(resolveTimeZone(undefined), 'Europe/Amsterdam')
  assert.equal(resolveTimeZone('Not/AZone'), 'Europe/Amsterdam')
  // First usable candidate wins; empties are skipped.
  assert.equal(resolveTimeZone('', undefined, 'UTC'), 'UTC')
  assert.equal(resolveTimeZone('Europe/Berlin'), 'Europe/Berlin')
})

test('formatLocalUpdatedAt tolerates an empty timezone instead of throwing', () => {
  // Regression: an empty zone previously threw RangeError and 500'd /dashboard.
  assert.equal(formatLocalUpdatedAt(INSTANT, ''), '23:14')
  assert.equal(formatLocalIso(INSTANT, ''), '2026-06-03T23:14:09')
})

test('buildDashboardPayload emits schemaVersion 2 with bounded services', async () => {
  const payload = await buildDashboardPayload(
    INSTANT,
    [
      {
        id: 'test',
        async getService() {
          return {
            id: 'github',
            kind: 'code-host',
            provider: 'github',
            label: 'GitHub',
            status: 'ok',
            counters: [{ id: 'issues', label: 'Issues', value: 3 }],
          }
        },
      },
      {
        id: 'empty',
        async getService() {
          return null
        },
      },
    ],
    'Europe/Amsterdam',
  )

  assert.equal(payload.schemaVersion, 2)
  assert.deepEqual(payload.services, [
    {
      id: 'github',
      kind: 'code-host',
      provider: 'github',
      label: 'GitHub',
      status: 'ok',
      counters: [{ id: 'issues', label: 'Issues', value: 3 }],
    },
  ])
  assert.equal(payload.updatedAt, '2026-06-03T21:14:09.000Z')
  assert.equal(payload.updatedAtLocal, '23:14')
  assert.equal(payload.updatedAtLocalIso, '2026-06-03T23:14:09')
  assert.equal('github' in payload, false)
  assert.equal('claude' in payload, false)
  assert.equal('codex' in payload, false)
})
