/* Weekly daily-limit tick for the 7-day usage bar.
 *
 * Given the current usedPercent and how long until the rolling 7-day window
 * resets, compute a recommended marker position (0..100) so the remaining
 * weekly budget can be spread evenly across the remaining usage days. Two modes:
 *
 *   ceiling   - today's recommended ceiling: current usage plus an equal share
 *               of the remaining budget, spread over the remaining working days
 *               (today counts as a whole day; a non-working today contributes
 *               nothing, so the tick sits at the current usage).
 *   even-pace - where usage "should" be by now if spread evenly across the
 *               window: elapsed working time / total working time * 100.
 *
 * Working time is measured as a continuous integral over the exact interval
 * (not by counting calendar dates): the 168-hour window rarely aligns to local
 * midnight, so it spans ~8 local dates and a per-date count would miscount the
 * working days. The integral also keeps the even-pace tick moving smoothly
 * instead of jumping once per day. */

const DAY_MS = 24 * 60 * 60 * 1000
const WINDOW_MS = 7 * DAY_MS
const STEP_MS = 30 * 60 * 1000 // integration step; far finer than one bar segment

export type WeekTickMode = 'ceiling' | 'even-pace'

const WEEKDAY_INDEX: Record<string, number> = {
  sun: 0,
  mon: 1,
  tue: 2,
  wed: 3,
  thu: 4,
  fri: 5,
  sat: 6,
}

/* Resolve WEEK_TICK_MODE: empty/unset disables the tick; anything other than the
 * two known modes is treated as disabled with a warning. */
export function resolveWeekTickMode(raw: string | undefined): WeekTickMode | undefined {
  const value = raw?.trim().toLowerCase()
  if (!value) return undefined
  if (value === 'ceiling' || value === 'even-pace') return value
  console.warn(`[dashboard] WEEK_TICK_MODE "${raw}" is invalid; the weekly tick is disabled`)
  return undefined
}

/* Resolve WORK_DAYS into a set of weekday numbers (0=Sun..6=Sat). Empty/unset —
 * or all-invalid input — means every day counts. Accepts comma-separated 3-letter
 * abbreviations (mon,tue,...), case-insensitive. */
export function resolveWorkDays(raw: string | undefined): Set<number> {
  const all = new Set([0, 1, 2, 3, 4, 5, 6])
  const trimmed = raw?.trim()
  if (!trimmed) return all
  const days = new Set<number>()
  for (const part of trimmed.split(',')) {
    const key = part.trim().toLowerCase().slice(0, 3)
    const index = Object.hasOwn(WEEKDAY_INDEX, key) ? WEEKDAY_INDEX[key] : undefined
    if (index === undefined) {
      if (part.trim()) console.warn(`[dashboard] WORK_DAYS entry "${part.trim()}" is not a weekday; ignored`)
      continue
    }
    days.add(index)
  }
  return days.size > 0 ? days : all
}

export const WEEK_TICK_MODE = resolveWeekTickMode(process.env.WEEK_TICK_MODE)
export const WORK_DAYS = resolveWorkDays(process.env.WORK_DAYS)

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min
  return value < min ? min : value > max ? max : value
}

function makeWeekdayAt(timeZone: string): (ms: number) => number {
  const fmt = new Intl.DateTimeFormat('en-US', { timeZone, weekday: 'short' })
  return (ms: number) => {
    const name = fmt.format(new Date(ms)).toLowerCase()
    return Object.hasOwn(WEEKDAY_INDEX, name) ? WEEKDAY_INDEX[name] : new Date(ms).getUTCDay()
  }
}

/* Instant of the most recent local midnight at or before `nowMs`. Sub-second and
 * rare same-day DST transitions are ignored — far below one bar segment. */
function startOfLocalDay(nowMs: number, timeZone: string): number {
  const fmt = new Intl.DateTimeFormat('en-GB', {
    timeZone,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  })
  const parts = fmt.formatToParts(new Date(nowMs))
  const get = (type: string) => Number(parts.find((p) => p.type === type)?.value ?? '0')
  let hour = get('hour')
  if (hour === 24) hour = 0
  const msIntoDay = ((hour * 60 + get('minute')) * 60 + get('second')) * 1000 + (nowMs % 1000)
  return nowMs - msIntoDay
}

/* Continuous count, in days, of the working time inside [fromMs, toMs). */
function measureWorkingDays(
  fromMs: number,
  toMs: number,
  workDays: Set<number>,
  weekdayAt: (ms: number) => number,
): number {
  if (toMs <= fromMs) return 0
  if (workDays.size >= 7) return (toMs - fromMs) / DAY_MS
  let working = 0
  for (let t = fromMs; t < toMs; t += STEP_MS) {
    const end = Math.min(t + STEP_MS, toMs)
    if (workDays.has(weekdayAt(t + (end - t) / 2))) working += end - t
  }
  return working / DAY_MS
}

export type WeekTickParams = {
  usedPercent: number
  resetInSeconds: number
  mode: WeekTickMode | undefined
  workDays: Set<number>
  timeZone: string
  now: number // epoch ms
}

/* Returns the marker position 0..100, or undefined when the tick is off or the
 * inputs cannot produce a meaningful value. */
export function computeWeekTick(params: WeekTickParams): number | undefined {
  const { mode, resetInSeconds, timeZone, workDays, now } = params
  if (!mode) return undefined
  if (!Number.isFinite(resetInSeconds) || resetInSeconds <= 0) return undefined

  const used = clamp(params.usedPercent, 0, 100)
  const reset = now + resetInSeconds * 1000
  const start = reset - WINDOW_MS
  const weekdayAt = makeWeekdayAt(timeZone)

  if (mode === 'even-pace') {
    const total = measureWorkingDays(start, reset, workDays, weekdayAt)
    if (total <= 0) return undefined
    const elapsed = measureWorkingDays(start, clamp(now, start, reset), workDays, weekdayAt)
    return clamp((elapsed / total) * 100, 0, 100)
  }

  // ceiling: on a non-working day the recommendation is to use nothing more, so
  // the tick sits at the current usage.
  if (workDays.size < 7 && !workDays.has(weekdayAt(now))) return used

  const from = clamp(startOfLocalDay(now, timeZone), start, reset)
  const remaining = measureWorkingDays(from, reset, workDays, weekdayAt)
  if (remaining <= 0) return used
  return clamp(used + (100 - used) / remaining, 0, 100)
}
