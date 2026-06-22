import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  computeWeekTick,
  resolveWeekTickMode,
  resolveWorkDays,
  type WeekTickParams,
} from './week-tick.js'

const DAY = 24 * 60 * 60 * 1000
const ALL_DAYS = resolveWorkDays('')
const WORK_WEEK = resolveWorkDays('mon,tue,thu,fri') // {Mon,Tue,Thu,Fri}

function approx(actual: number | undefined, expected: number, tol = 0.2) {
  assert.ok(actual != null, 'expected a tick value')
  assert.ok(Math.abs(actual - expected) <= tol, `expected ~${expected}, got ${actual}`)
}

// Midnight UTC on the next given weekday (0=Sun..6=Sat) at/after June 2026.
function weekdayMidnightUTC(weekday: number): number {
  let ms = Date.UTC(2026, 5, 1, 0, 0, 0)
  while (new Date(ms).getUTCDay() !== weekday) ms += DAY
  return ms
}

const base = (over: Partial<WeekTickParams>): WeekTickParams => ({
  usedPercent: 30,
  resetInSeconds: 4 * 86_400,
  mode: 'ceiling',
  workDays: ALL_DAYS,
  timeZone: 'UTC',
  now: weekdayMidnightUTC(2), // a Tuesday
  ...over,
})

test('resolveWeekTickMode accepts the two modes, else undefined', () => {
  assert.equal(resolveWeekTickMode('ceiling'), 'ceiling')
  assert.equal(resolveWeekTickMode(' Even-Pace '), 'even-pace')
  assert.equal(resolveWeekTickMode(''), undefined)
  assert.equal(resolveWeekTickMode(undefined), undefined)
  assert.equal(resolveWeekTickMode('weekly'), undefined)
})

test('resolveWorkDays parses abbreviations and falls back to all 7', () => {
  assert.deepEqual([...resolveWorkDays('mon,tue,thu,fri')].sort(), [1, 2, 4, 5])
  assert.deepEqual([...resolveWorkDays('MON, Fri')].sort(), [1, 5])
  assert.equal(resolveWorkDays('').size, 7)
  assert.equal(resolveWorkDays('garbage').size, 7)
})

test('no tick when mode is off or reset is non-positive', () => {
  assert.equal(computeWeekTick(base({ mode: undefined })), undefined)
  assert.equal(computeWeekTick(base({ mode: 'even-pace', resetInSeconds: 0 })), undefined)
})

test('even-pace returns the elapsed fraction of the window', () => {
  // reset in 2 days => start was 5 days ago => 5/7 elapsed.
  approx(computeWeekTick(base({ mode: 'even-pace', resetInSeconds: 2 * 86_400 })), (5 / 7) * 100)
  // exactly mid-window => 50%.
  approx(computeWeekTick(base({ mode: 'even-pace', resetInSeconds: 3.5 * 86_400 })), 50)
})

test('ceiling spreads the remaining budget over the remaining days (all 7)', () => {
  // used 30, reset in 4 days, today counts fully => 70 / 4 = 17.5 => ~47.5.
  approx(computeWeekTick(base({})), 47.5)
})

test('ceiling counts only working days when WORK_DAYS is set', () => {
  // Tue 00:00, reset Sat 00:00. Working days in [Tue,Sat): Tue, Thu, Fri = 3.
  // 70 / 3 = 23.3 => ~53.3 (matches the design example).
  approx(computeWeekTick(base({ workDays: WORK_WEEK })), 53.3)
})

test('ceiling on a non-working day sits at the current usage', () => {
  const tick = computeWeekTick(base({ workDays: WORK_WEEK, now: weekdayMidnightUTC(3) })) // Wed
  approx(tick, 30, 0.01)
})

test('ceiling never exceeds 100', () => {
  const tick = computeWeekTick(base({ usedPercent: 95, resetInSeconds: 12 * 3_600 }))
  assert.ok(tick != null && tick <= 100)
})
