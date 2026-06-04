import type { FastifyInstance } from 'fastify'
import { createCodeHostAdapters } from '../services/code-host.adapters.js'
import {
  DASHBOARD_SCHEMA_VERSION,
  type DashboardService,
  type DashboardServiceAdapter,
} from '../services/dashboard-service.js'
import { createUsageAdapters } from '../services/usage.adapters.js'

const DASHBOARD_TIME_ZONE = process.env.DASHBOARD_TIME_ZONE
  ?? process.env.TZ
  ?? 'Europe/Amsterdam'

export function formatLocalUpdatedAt(
  now: Date,
  timeZone: string = DASHBOARD_TIME_ZONE,
): string {
  return new Intl.DateTimeFormat('en-GB', {
    timeZone,
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
    timeZone,
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

export async function buildDashboardPayload(
  now: Date,
  adapters: DashboardServiceAdapter[],
  timeZone: string = DASHBOARD_TIME_ZONE,
): Promise<DashboardPayload> {
  const services = (await Promise.all(adapters.map((adapter) => adapter.getService())))
    .filter((service): service is DashboardService => service !== null)

  return {
    schemaVersion: DASHBOARD_SCHEMA_VERSION,
    services,
    updatedAt: now.toISOString(),
    updatedAtLocal: formatLocalUpdatedAt(now, timeZone),
    updatedAtLocalIso: formatLocalIso(now, timeZone),
  }
}

export async function dashboardRoute(app: FastifyInstance) {
  app.get('/dashboard', async (_req, reply) => {
    const now = new Date()
    const body = await buildDashboardPayload(now, [
      ...createCodeHostAdapters(),
      ...createUsageAdapters(),
    ])

    return reply.send(body)
  })
}
