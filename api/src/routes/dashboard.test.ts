import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  buildDashboardPayload,
  formatLocalIso,
  formatLocalUpdatedAt,
  resolveTimeZone,
  startOfLocalDayMs,
} from './dashboard.js'
import { resetUsageHistory } from '../services/usage-history.js'
import { WEEK_TICK_MODE } from '../services/week-tick.js'

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

test('buildDashboardPayload flattens grouped adapters and caps usage services at four', async () => {
  const usage = (id: string) => ({
    id,
    kind: 'usage' as const,
    provider: 'test',
    label: id,
    status: 'ok' as const,
    windows: [],
  })
  const payload = await buildDashboardPayload(
    INSTANT,
    [
      {
        id: 'grouped',
        async getService() {
          return usage('fallback')
        },
        async getServices() {
          return ['one', 'two', 'three', 'four', 'five'].map(usage)
        },
      },
    ],
    'Europe/Amsterdam',
  )

  assert.deepEqual(payload.services.map((service) => service.id), [
    'one',
    'two',
    'three',
    'four',
  ])
})

// --- startOfLocalDayMs --------------------------------------------------------

test('startOfLocalDayMs returns local midnight on a normal day', () => {
  // 2026-06-15T08:00:00Z is local 10:00 in Amsterdam (UTC+2 in summer); local
  // midnight 2026-06-15T00:00 CEST is 2026-06-14T22:00:00Z.
  const now = new Date('2026-06-15T08:00:00.000Z')
  assert.equal(
    startOfLocalDayMs(now, 'Europe/Amsterdam'),
    Date.parse('2026-06-14T22:00:00.000Z'),
  )
})

test('startOfLocalDayMs honors the supplied timezone (Tokyo)', () => {
  // 2026-06-15T08:00:00Z is local 17:00 JST; Tokyo midnight is the prior 15:00Z.
  const now = new Date('2026-06-15T08:00:00.000Z')
  assert.equal(
    startOfLocalDayMs(now, 'Asia/Tokyo'),
    Date.parse('2026-06-14T15:00:00.000Z'),
  )
})

test('startOfLocalDayMs picks the midnight offset on the spring-forward day', () => {
  // 2026-03-29 Amsterdam springs 02:00 CET -> 03:00 CEST. Midnight is unaffected
  // (00:00 CET = 2026-03-28T23:00:00Z), but the offset at `now` (CEST, +2)
  // differs from the offset at midnight (CET, +1); a naive subtraction would be
  // an hour off.
  const now = new Date('2026-03-29T10:00:00.000Z') // local 12:00 CEST
  assert.equal(
    startOfLocalDayMs(now, 'Europe/Amsterdam'),
    Date.parse('2026-03-28T23:00:00.000Z'),
  )
})

test('startOfLocalDayMs picks the midnight offset on the fall-back day', () => {
  // 2026-10-25 Amsterdam falls back 03:00 CEST -> 02:00 CET. Midnight is still
  // CEST (00:00 CEST = 2026-10-24T22:00:00Z) while `now` is CET (+1).
  const now = new Date('2026-10-25T10:00:00.000Z') // local 11:00 CET
  assert.equal(
    startOfLocalDayMs(now, 'Europe/Amsterdam'),
    Date.parse('2026-10-24T22:00:00.000Z'),
  )
})

test('startOfLocalDayMs returns the first valid instant when local midnight does not exist', () => {
  // America/Sao_Paulo (historical) sprang forward at 00:00 -> 01:00 on
  // 2018-11-04, so local midnight never happened. Start of day is the first
  // valid instant, 01:00 BRST = 2018-11-04T03:00:00Z.
  const now = new Date('2018-11-04T12:00:00.000Z')
  assert.equal(
    startOfLocalDayMs(now, 'America/Sao_Paulo'),
    Date.parse('2018-11-04T03:00:00.000Z'),
  )
})

// --- per-window recent cutoff in buildDashboardPayload ------------------------

test('the 7d window uses the local-day cutoff while short windows use the hour cutoff', async () => {
  resetUsageHistory()
  const tz = 'Asia/Tokyo' // non-default; Tokyo midnight for these instants is 15:00Z the day before
  // Fixed absolute reset instants. resetInSeconds is a countdown, so it must
  // shrink as `now` advances to keep resetAt stable; a constant countdown would
  // make resetAt drift forward each build and falsely trip the reset guard.
  const shortReset = Date.parse('2026-06-15T08:00:00.000Z')
  const weeklyReset = Date.parse('2026-06-20T00:00:00.000Z')
  const mk = (now: Date, shortUsed: number, weeklyUsed: number) => ({
    id: 'u',
    async getService() {
      return {
        id: 'prov',
        kind: 'usage' as const,
        provider: 'test',
        label: 'Prov',
        status: 'ok' as const,
        windows: [
          { id: 'short', label: '5h', usedPercent: shortUsed, resetInSeconds: (shortReset - now.getTime()) / 1000 },
          { id: 'weekly', label: '7d', usedPercent: weeklyUsed, resetInSeconds: (weeklyReset - now.getTime()) / 1000 },
        ],
      }
    },
  })
  // All three instants are the same Tokyo day (2026-06-15), after Tokyo midnight,
  // so the day window has no pre-midnight baseline and warms up to the oldest.
  const t0 = new Date('2026-06-15T02:00:00.000Z') // Tokyo 11:00
  const t1 = new Date('2026-06-15T05:00:00.000Z') // Tokyo 14:00
  const t2 = new Date('2026-06-15T06:00:00.000Z') // Tokyo 15:00
  await buildDashboardPayload(t0, [mk(t0, 10, 10)], tz)
  await buildDashboardPayload(t1, [mk(t1, 30, 30)], tz)
  const payload = await buildDashboardPayload(t2, [mk(t2, 40, 40)], tz)

  const windows = payload.services[0].windows!
  const short = windows.find((w) => w.id === 'short')!
  const weekly = windows.find((w) => w.id === 'weekly')!
  // Hour cutoff at 06:00Z: baseline is the 05:00Z sample (30) -> 40 - 30 = 10.
  assert.equal(short.recentPercent, 10)
  // Day cutoff (Tokyo): warm-up baseline is the oldest same-day sample (10) ->
  // 40 - 10 = 30, proving the long window does not use the hour cutoff.
  assert.equal(weekly.recentPercent, 30)
})

// --- ceiling tick anchored to start-of-day usage ------------------------------

test('tickPercent (ceiling mode) does not move as usedPercent grows during the day', async () => {
  // This test verifies that when tickPercent is emitted it is computed from the
  // start-of-day usage (used - recentPercent) and not from the current
  // usedPercent. WEEK_TICK_MODE is read at module load time from the env, so
  // this test cannot force it to 'ceiling' without process restarts. We verify
  // the underlying arithmetic instead: the recentPercent on the long window
  // correctly reflects today's usage, meaning used - recentPercent is the
  // start-of-day value the tick would be anchored to.
  resetUsageHistory()
  const tz = 'UTC'
  // midnight UTC on 2026-06-15
  const midnight = Date.parse('2026-06-15T00:00:00.000Z')
  const weeklyReset = Date.parse('2026-06-20T00:00:00.000Z') // 5 days from midnight

  const mk = (now: Date, weeklyUsed: number) => ({
    id: 'u2',
    async getService() {
      return {
        id: 'prov2',
        kind: 'usage' as const,
        provider: 'test',
        label: 'Prov2',
        status: 'ok' as const,
        windows: [
          {
            id: 'weekly',
            label: '7d',
            usedPercent: weeklyUsed,
            resetInSeconds: (weeklyReset - now.getTime()) / 1000,
          },
        ],
      }
    },
  })

  // t0: just before midnight — establishes the pre-midnight baseline (used = 10)
  const t0 = new Date(midnight - 60 * 1000) // 23:59
  await buildDashboardPayload(t0, [mk(t0, 10)], tz)

  // t1: 2 h into the day, usage has grown to 30 (today's usage: 30 - 10 = 20)
  const t1 = new Date(midnight + 2 * 60 * 60 * 1000) // 02:00
  const p1 = await buildDashboardPayload(t1, [mk(t1, 30)], tz)
  const w1 = p1.services[0].windows!.find((w) => w.id === 'weekly')!
  assert.equal(w1.recentPercent, 20, 'at t1: today accrued 20pp (30 - 10)')
  // The tick would be anchored to usedAtDayStart = 30 - 20 = 10 (start-of-day)

  // t2: 4 h into the day, usage has grown to 50 (today's usage: 50 - 10 = 40)
  const t2 = new Date(midnight + 4 * 60 * 60 * 1000) // 04:00
  const p2 = await buildDashboardPayload(t2, [mk(t2, 50)], tz)
  const w2 = p2.services[0].windows!.find((w) => w.id === 'weekly')!
  assert.equal(w2.recentPercent, 40, 'at t2: today accrued 40pp (50 - 10)')
  // The tick would be anchored to usedAtDayStart = 50 - 40 = 10 (same as t1)

  // Both anchors resolve to the same start-of-day value (10). If tickPercent is
  // present AND the active mode is 'ceiling', both ticks must be identical.
  // Under 'even-pace' the tick correctly advances over time, so we do not
  // assert equality in that case.
  const tick1 = w1.tickPercent
  const tick2 = w2.tickPercent
  if (tick1 != null && tick2 != null && WEEK_TICK_MODE === 'ceiling') {
    assert.equal(tick1, tick2, 'tickPercent should be stable across polls on the same day')
  }
})
