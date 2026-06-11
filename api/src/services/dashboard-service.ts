export const DASHBOARD_SCHEMA_VERSION = 2

export type DashboardServiceKind = 'code-host' | 'usage'
export type DashboardServiceStatus = 'ok' | 'unavailable' | 'auth_error' | 'error'

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
}
