import type { DashboardResetCredits } from './dashboard-service.js'

const MAX_INTEGER = 2_147_483_647

function record(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown> : null
}

function count(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= MAX_INTEGER
}

function secondsUntil(expiryMs: number | null, nowMs: number): number | null {
  if (expiryMs === null) return null
  const seconds = Math.ceil((expiryMs - nowMs) / 1000)
  return seconds > 0 && seconds <= MAX_INTEGER ? seconds : null
}

// Undocumented Claude inventory. Do not turn a malformed inventory into a
// confirmed zero; quota windows and spend remain independent of this parser.
export function claudeResetCredits(value: unknown, nowMs = Date.now()): DashboardResetCredits | undefined {
  const inventory = record(value)
  if (inventory?.eligible !== true || !Array.isArray(inventory.grants) || inventory.grants.length > 200) return undefined
  let availableCount = 0
  let earliest: number | null = null
  for (const value of inventory.grants) {
    const grant = record(value)
    if (!grant || !count(grant.resets_left) || !count(grant.resets_total) ||
        grant.resets_left > grant.resets_total || typeof grant.paused !== 'boolean' ||
        typeof grant.starts_at !== 'string' || typeof grant.ends_at !== 'string') return undefined
    const start = Date.parse(grant.starts_at)
    const end = Date.parse(grant.ends_at)
    if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return undefined
    if (grant.paused || grant.resets_left === 0 || start > nowMs || end <= nowMs) continue
    availableCount += grant.resets_left
    if (!count(availableCount)) return undefined
    earliest = earliest === null ? end : Math.min(earliest, end)
  }
  return { availableCount, nextExpiresInSeconds: secondsUntil(earliest, nowMs) }
}

// App-server inventory lives at the response root, independently of quota
// buckets. Its count is authoritative: detail rows may be absent or capped.
export function codexResetCredits(value: unknown, nowMs = Date.now()): DashboardResetCredits | undefined {
  const inventory = record(value)
  if (!inventory || !count(inventory.availableCount)) return undefined
  let earliest: number | null = null
  if (inventory.availableCount > 0 && Array.isArray(inventory.credits)) {
    for (const value of inventory.credits) {
      const credit = record(value)
      if (credit?.status !== 'available' || typeof credit.expiresAt !== 'number' ||
          !Number.isSafeInteger(credit.expiresAt)) continue
      const expiryMs = credit.expiresAt * 1000
      if (!Number.isSafeInteger(expiryMs) || expiryMs <= nowMs) continue
      earliest = earliest === null ? expiryMs : Math.min(earliest, expiryMs)
    }
  }
  return { availableCount: inventory.availableCount, nextExpiresInSeconds: secondsUntil(earliest, nowMs) }
}
