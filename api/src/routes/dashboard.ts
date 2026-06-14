import type { FastifyInstance } from 'fastify'
import type { DashboardCoordinator } from '../dashboard-coordinator.js'
import { createCodeHostAdapters } from '../services/code-host.adapters.js'
import {
  DASHBOARD_SCHEMA_VERSION,
  type DashboardService,
  type DashboardServiceAdapter,
} from '../services/dashboard-service.js'
import { createUsageAdapters } from '../services/usage.adapters.js'

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

export type DashboardPayload = {
  schemaVersion: typeof DASHBOARD_SCHEMA_VERSION
  services: DashboardService[]
  updatedAt: string
  updatedAtLocal: string
  updatedAtLocalIso: string
}

const MAX_USAGE_SERVICES = 4

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
