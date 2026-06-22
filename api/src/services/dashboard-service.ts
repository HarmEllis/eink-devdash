export const DASHBOARD_SCHEMA_VERSION = 2

export type DashboardServiceKind = 'code-host' | 'usage'
export type DashboardServiceStatus = 'ok' | 'unavailable' | 'auth_error' | 'error'
export type DashboardServiceIcon = 'spark' | 'ring' | 'lift' | 'diamond' | 'generic'

export type DashboardCounter = {
  id: string
  label: string
  value: number
  alert?: boolean
}

export type DashboardUsageWindow = {
  id: string
  label: string
  used?: number
  limit?: number
  usedPercent?: number
  resetInSeconds?: number
  resetsAt?: number | null
  reachedLimit?: boolean
  // Additive (schema stays 2). Portion of the window's usage accrued in the last
  // hour (0..effective usedPercent); the firmware renders it as a grey slice.
  recentPercent?: number
  // Recommended daily-limit marker position (0..100) on the long/weekly window;
  // emitted only when WEEK_TICK_MODE is set. Older firmware ignores it.
  tickPercent?: number
}

export type DashboardMetric = {
  id: string
  label: string
  value: number
  unit?: string
  usedPercent?: number
  limit?: number
  resetInSeconds?: number
  // Preformatted, locale-aware, ASCII-only amount string (<=15 bytes) for the
  // firmware to draw verbatim. The device prefers this over formatting `value`
  // itself; absent it falls back to numeric formatting.
  valueText?: string
}

export type DashboardService = {
  id: string
  kind: DashboardServiceKind
  provider: string
  label: string
  // Abstract display primitive, not a provider identity. Unknown/missing values
  // fall back to the generic diamond in firmware.
  icon?: DashboardServiceIcon
  status: DashboardServiceStatus
  counters?: DashboardCounter[]
  windows?: DashboardUsageWindow[]
  metrics?: DashboardMetric[]
  source?: string
  planType?: string | null
}

export type DashboardServiceAdapter = {
  id: string
  getService(signal?: AbortSignal): Promise<DashboardService | null>
  // Providers with shared quota groups can emit multiple normal usage tiles
  // from one upstream fetch.
  getServices?(signal?: AbortSignal): Promise<DashboardService[]>
}
