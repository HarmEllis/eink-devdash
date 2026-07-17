import type { FastifyInstance } from 'fastify'
import type { DashboardCoordinator } from '../dashboard-coordinator.js'
import { createCodeHostAdapters } from '../services/code-host.adapters.js'
import {
  DASHBOARD_SCHEMA_VERSION,
  type DashboardService,
  type DashboardServiceAdapter,
  type DashboardUsageWindow,
} from '../services/dashboard-service.js'
import { createUsageAdapters } from '../services/usage.adapters.js'
import { recordAndComputeRecent, usageHistoryKey } from '../services/usage-history.js'
import { WEEK_TICK_MODE, WORK_DAYS, computeWeekTick } from '../services/week-tick.js'

const DEFAULT_TIME_ZONE = 'Europe/Amsterdam'

function isValidTimeZone(timeZone: string): boolean {
  try {
    // Throws RangeError on an unknown/empty zone.
    new Intl.DateTimeFormat('en-GB', { timeZone })
    return true
  } catch {
    return false
  }
}

/* Pick the first usable IANA zone from the candidates, falling back to the
 * default. Crucially this rejects the empty string: docker-compose passes
 * `DASHBOARD_TIME_ZONE=${DASHBOARD_TIME_ZONE:-}`, so an unset variable arrives
 * as "" (not undefined) and `??` would not skip it — Intl then throws
 * `RangeError: Invalid time zone specified: ` and the whole /dashboard 500s. */
export function resolveTimeZone(...candidates: (string | undefined)[]): string {
  for (const candidate of candidates) {
    const timeZone = candidate?.trim()
    if (timeZone && isValidTimeZone(timeZone)) return timeZone
  }
  return DEFAULT_TIME_ZONE
}

const DASHBOARD_TIME_ZONE = resolveTimeZone(
  process.env.DASHBOARD_TIME_ZONE,
  process.env.TZ,
)

export function formatLocalUpdatedAt(
  now: Date,
  timeZone: string = DASHBOARD_TIME_ZONE,
): string {
  return new Intl.DateTimeFormat('en-GB', {
    timeZone: resolveTimeZone(timeZone),
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).format(now)
}

/* Full local wall-clock timestamp without a timezone offset, e.g.
 * "2026-06-03T23:14:00". The firmware parses this to set its RTC clock so the
 * per-network quiet-hours window can be evaluated against local time. It is
 * deliberately offset-less: it encodes the dashboard's local wall time, which
 * is exactly what the user configures the quiet window against. updatedAtLocal
 * (HH:MM) stays for display/back-compat. */
export function formatLocalIso(
  now: Date,
  timeZone: string = DASHBOARD_TIME_ZONE,
): string {
  const parts = new Intl.DateTimeFormat('en-CA', {
    timeZone: resolveTimeZone(timeZone),
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  }).formatToParts(now)
  const get = (type: string) => parts.find((p) => p.type === type)?.value ?? ''
  // Some Intl implementations render midnight as hour "24" under hour12:false.
  const hour = get('hour') === '24' ? '00' : get('hour')
  return `${get('year')}-${get('month')}-${get('day')}T${hour}:${get('minute')}:${get('second')}`
}

/* Local wall-clock fields of `instant` in `timeZone`. Midnight is normalised
 * from hour "24" to 0 (some Intl implementations render it as 24 under h23). */
function wallClock(timeZone: string, instant: number) {
  const parts = new Intl.DateTimeFormat('en-CA', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hourCycle: 'h23',
  }).formatToParts(new Date(instant))
  const get = (type: string) => Number(parts.find((p) => p.type === type)?.value)
  const hour = get('hour')
  return {
    y: get('year'),
    m: get('month'),
    d: get('day'),
    h: hour === 24 ? 0 : hour,
    mi: get('minute'),
    s: get('second'),
  }
}

/* Offset (ms) of `timeZone` at `instant`: how far the local wall clock is ahead
 * of UTC, i.e. localWallTimeAsUtc - instant. */
function tzOffsetMs(timeZone: string, instant: number): number {
  const w = wallClock(timeZone, instant)
  return Date.UTC(w.y, w.m - 1, w.d, w.h, w.mi, w.s) - instant
}

/* Epoch ms of the start of `now`'s local calendar day in `timeZone` (00:00:00
 * local). DST-safe: the offset at local midnight can differ from the offset at
 * `now`, so a naive "subtract the formatted h/m/s" would shift the result by an
 * hour on transition days. We resolve the target wall time to an instant and
 * verify it maps back to local midnight; if 00:00 does not exist (a zone that
 * springs forward exactly at midnight) we return the first valid instant of the
 * day (the later candidate, e.g. 01:00 local). */
export function startOfLocalDayMs(now: Date, timeZone: string): number {
  const tz = resolveTimeZone(timeZone)
  const w = wallClock(tz, now.getTime())
  const guess = Date.UTC(w.y, w.m - 1, w.d, 0, 0, 0)
  const candidateA = guess - tzOffsetMs(tz, guess)
  const candidateB = guess - tzOffsetMs(tz, candidateA)
  const isLocalMidnight = (instant: number) => {
    const c = wallClock(tz, instant)
    return c.y === w.y && c.m === w.m && c.d === w.d && c.h === 0 && c.mi === 0 && c.s === 0
  }
  if (isLocalMidnight(candidateA)) return candidateA
  if (isLocalMidnight(candidateB)) return candidateB
  return Math.max(candidateA, candidateB)
}

export type DashboardPayload = {
  schemaVersion: typeof DASHBOARD_SCHEMA_VERSION
  services: DashboardService[]
  updatedAt: string
  updatedAtLocal: string
  updatedAtLocalIso: string
}

const MAX_USAGE_SERVICES = 4

// Long/weekly window ids across providers (Claude: 'weekly', Codex/Antigravity:
// 'long'). Only this window carries the recommended daily-limit tick.
const LONG_WINDOW_IDS = new Set(['weekly', 'long'])

/* Effective used percentage of a window, mirroring the firmware's
 * usage_window_percent: prefer usedPercent, else used/limit. */
function effectiveUsedPercent(window: DashboardUsageWindow): number | null {
  if (typeof window.usedPercent === 'number') {
    return Math.min(Math.max(window.usedPercent, 0), 100)
  }
  if (typeof window.used === 'number' && typeof window.limit === 'number' && window.limit > 0) {
    return Math.min(Math.max((window.used / window.limit) * 100, 0), 100)
  }
  return null
}

/* Attach the last-hour grey slice (recentPercent) to every usage window and the
 * recommended daily-limit tick (tickPercent) to the long/weekly window. Computed
 * here, in the single shared build path, so the firmware stays stateless. */
function enrichUsageWindows(services: DashboardService[], timeZone: string, now: Date): void {
  const nowMs = now.getTime()
  // The 7d/weekly slice means "today" (since local midnight); short windows keep
  // the last-hour slice. Compute the day cutoff once; short windows fall back to
  // recordAndComputeRecent's default (~1h ago).
  const dayCutoff = startOfLocalDayMs(now, timeZone)
  for (const service of services) {
    if (service.kind !== 'usage' || !service.windows) continue
    for (const window of service.windows) {
      const used = effectiveUsedPercent(window)
      if (used == null) continue

      // Only an absolute reset instant when there is a real countdown. A
      // fallback resetInSeconds of 0 (missing headers/quota) would otherwise
      // make resetAt mirror "now" and falsely trip the window-reset guard
      // against the baseline, flagging the entire usage as recent.
      const resetAt = window.resetInSeconds != null && window.resetInSeconds > 0
        ? nowMs + window.resetInSeconds * 1000
        : null
      const cutoff = LONG_WINDOW_IDS.has(window.id) ? dayCutoff : undefined
      const recent = recordAndComputeRecent(usageHistoryKey(service.id, window.id), used, resetAt, nowMs, cutoff)
      window.recentPercent = Math.min(Math.round(recent), Math.round(used))

      if (LONG_WINDOW_IDS.has(window.id) && window.resetInSeconds != null) {
        // Use the start-of-day usage as the tick baseline so the recommended
        // ceiling is computed once per day and stays stable as usage grows.
        // recentPercent is the usage accrued today, so used - recent gives the
        // usage level at local midnight. For even-pace mode usedPercent is not
        // used in the formula, so this has no effect there.
        const usedAtDayStart = Math.max(0, used - recent)
        const tick = computeWeekTick({
          usedPercent: usedAtDayStart,
          resetInSeconds: window.resetInSeconds,
          mode: WEEK_TICK_MODE,
          workDays: WORK_DAYS,
          timeZone,
          now: nowMs,
        })
        if (tick != null) window.tickPercent = Math.round(tick)
      }
    }
  }
}

export function createDashboardAdapters(): DashboardServiceAdapter[] {
  return [
    ...createCodeHostAdapters(),
    ...createUsageAdapters(),
  ]
}

export async function buildDashboardPayload(
  now: Date,
  adapters: DashboardServiceAdapter[],
  timeZone: string = DASHBOARD_TIME_ZONE,
  opts: { signal?: AbortSignal } = {},
): Promise<DashboardPayload> {
  const results = await Promise.all(adapters.map(async (adapter) => {
    if (adapter.getServices) return adapter.getServices(opts.signal)
    const service = await adapter.getService(opts.signal)
    return service ? [service] : []
  }))
  let usageCount = 0
  const services = results.flat().filter((service) => {
    if (service.kind !== 'usage') return true
    usageCount += 1
    return usageCount <= MAX_USAGE_SERVICES
  })

  enrichUsageWindows(services, resolveTimeZone(timeZone), now)

  return {
    schemaVersion: DASHBOARD_SCHEMA_VERSION,
    services,
    updatedAt: now.toISOString(),
    updatedAtLocal: formatLocalUpdatedAt(now, timeZone),
    updatedAtLocalIso: formatLocalIso(now, timeZone),
  }
}

export type DashboardRouteOptions = {
  // Inject adapters in tests; production falls back to the real set so the
  // route stays self-contained when registered with no options.
  adapters?: DashboardServiceAdapter[]
  coordinator?: DashboardCoordinator
}

export async function dashboardRoute(app: FastifyInstance, opts: DashboardRouteOptions = {}) {
  const adapters = opts.coordinator ? null : opts.adapters ?? createDashboardAdapters()
  app.get('/dashboard', async (_req, reply) => {
    const body = opts.coordinator
      ? await opts.coordinator.getDashboard()
      : await buildDashboardPayload(new Date(), adapters!)

    return reply.send(body)
  })
}
