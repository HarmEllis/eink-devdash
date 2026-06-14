import type { DashboardMetric, DashboardService, DashboardServiceAdapter } from './dashboard-service.js'
import { getClaudeUsage, type ExtraUsage } from './claude.service.js'
import { getCodexUsage } from './codex.service.js'
import { getAntigravityUsage, type AntigravityUsage } from './antigravity.service.js'
import { DASHBOARD_LOCALE, currencySymbol, formatAmount, isSupportedCurrency } from './currency.js'

type ClaudeRateLimit = {
  used: number
  limit: number
  resetInSeconds: number
}

type ClaudeUsage = {
  fiveHour: ClaudeRateLimit
  weekly: ClaudeRateLimit
  authError: boolean
  extraUsage?: ExtraUsage | null
}

type CodexLimitReached = 'short' | 'long' | null
type CodexStatus = 'ok' | 'unavailable' | 'error'

type CodexWindow = {
  usedPercent: number
  label: string
  resetsAt: number | null
  resetInSeconds: number
}

type CodexUsage = {
  status: CodexStatus
  source: string
  planType: string | null
  short: CodexWindow
  long: CodexWindow
  reachedLimit: CodexLimitReached
  spend?: number | null
}

type UsageAdapterOptions<TUsage> = {
  getUsage?: (signal?: AbortSignal) => Promise<TUsage>
}

// Extra-usage bar: emit the `extraUsage` metric only when there is actual
// overage spend, so the firmware row stays hidden by default. The device draws
// a currency symbol (from `unit`), a bar from `usedPercent` (share of the
// monthly cap consumed) and the preformatted `valueText`. When `percent` is
// absent (env override) the bar falls back to an amount-capped fill on the
// device. `valueText` is the locale-aware, ASCII-only amount built centrally
// here so the Claude-live path and both env overrides share one locale.
// NB: a new metric id — `spend` is retired because released schema-2 firmware
// rendered `spend.value` as both `$` text and the bar %, which would misrender
// the new fractional, currency-aware value.
function extraUsageMetric(extra: ExtraUsage | null | undefined): DashboardMetric | undefined {
  // Authoritative gate: omit without spend OR for any currency we cannot both
  // convert and render. Unknown currencies are never emitted (and never shown
  // as `$`), regardless of how the ExtraUsage was constructed.
  if (!extra || !(extra.amount > 0) || !isSupportedCurrency(extra.currency)) return undefined
  const metric: DashboardMetric = {
    id: 'extraUsage',
    label: currencySymbol(extra.currency),
    value: extra.amount,
    valueText: formatAmount(extra.amount, extra.currency, DASHBOARD_LOCALE),
    unit: extra.currency,
  }
  if (extra.percent != null) metric.usedPercent = extra.percent
  if (extra.limit != null && extra.limit > 0) metric.limit = extra.limit
  return metric
}

export function serviceFromClaudeUsage(usage: ClaudeUsage): DashboardService {
  const service: DashboardService = {
    id: 'claude',
    kind: 'usage',
    provider: 'claude',
    label: 'Claude',
    icon: 'spark',
    status: usage.authError ? 'auth_error' : 'ok',
    windows: [
      {
        id: 'fiveHour',
        label: '5h',
        used: usage.fiveHour.used,
        limit: usage.fiveHour.limit,
        resetInSeconds: usage.fiveHour.resetInSeconds,
      },
      {
        id: 'weekly',
        label: '7d',
        used: usage.weekly.used,
        limit: usage.weekly.limit,
        resetInSeconds: usage.weekly.resetInSeconds,
      },
    ],
  }

  const metric = extraUsageMetric(usage.extraUsage)
  if (metric) service.metrics = [metric]

  return service
}

export function serviceFromCodexUsage(usage: CodexUsage): DashboardService {
  const service: DashboardService = {
    id: 'codex',
    kind: 'usage',
    provider: 'codex',
    label: 'Codex',
    icon: 'ring',
    status: usage.status,
    source: usage.source,
    planType: usage.planType,
    windows: [
      {
        id: 'short',
        label: usage.short.label,
        usedPercent: usage.short.usedPercent,
        resetsAt: usage.short.resetsAt,
        resetInSeconds: usage.short.resetInSeconds,
        reachedLimit: usage.reachedLimit === 'short',
      },
      {
        id: 'long',
        label: usage.long.label,
        usedPercent: usage.long.usedPercent,
        resetsAt: usage.long.resetsAt,
        resetInSeconds: usage.long.resetInSeconds,
        reachedLimit: usage.reachedLimit === 'long',
      },
    ],
  }

  // Codex has no live spend source (ChatGPT-auth exposes only a remaining-credit
  // snapshot), so the amount comes from CODEX_OVERAGE_USD with no percent/limit.
  const metric = extraUsageMetric(
    usage.spend != null && usage.spend > 0
      ? { amount: usage.spend, percent: null, limit: null, currency: 'USD' }
      : null,
  )
  if (metric) service.metrics = [metric]

  return service
}

function antigravityWindows(
  short: AntigravityUsage['short'],
  long: AntigravityUsage['long'],
  reachedLimit: AntigravityUsage['reachedLimit'],
) {
  return [
    {
      id: 'short',
      label: short.label,
      usedPercent: short.usedPercent,
      resetsAt: short.resetsAt,
      resetInSeconds: short.resetInSeconds,
      reachedLimit: reachedLimit === 'short',
    },
    {
      id: 'long',
      label: long.label,
      usedPercent: long.usedPercent,
      resetsAt: long.resetsAt,
      resetInSeconds: long.resetInSeconds,
      reachedLimit: reachedLimit === 'long',
    },
  ]
}

export function serviceFromAntigravityUsage(usage: AntigravityUsage): DashboardService[] {
  const groups = usage.groups.length > 0
    ? usage.groups
    : [{
        id: 'gemini' as const,
        label: 'Antigravity',
        short: usage.short,
        long: usage.long,
        reachedLimit: usage.reachedLimit,
      }]

  return groups.map((group, index) => {
    const service: DashboardService = {
      id: index === 0 ? 'antigravity' : `antigravity-${group.id}`,
      kind: 'usage',
      provider: 'antigravity',
      label: group.label,
      icon: 'lift',
      status: usage.status,
      windows: antigravityWindows(group.short, group.long, group.reachedLimit),
    }

    if (index === 0) {
      const metric = extraUsageMetric(
        usage.spend != null && usage.spend > 0
          ? { amount: usage.spend, percent: null, limit: null, currency: 'USD' }
          : null,
      )
      if (metric) service.metrics = [metric]
    }

    return service
  })
}

export function createClaudeUsageAdapter(
  options: UsageAdapterOptions<ClaudeUsage> = {},
): DashboardServiceAdapter {
  return {
    id: 'claude',
    async getService(signal?: AbortSignal) {
      const usage = await (options.getUsage ?? getClaudeUsage)(signal)
      return serviceFromClaudeUsage(usage)
    },
  }
}

export function createCodexUsageAdapter(
  options: UsageAdapterOptions<CodexUsage> = {},
): DashboardServiceAdapter {
  return {
    id: 'codex',
    async getService(signal?: AbortSignal) {
      const usage = await (options.getUsage ?? getCodexUsage)(signal)
      return serviceFromCodexUsage(usage)
    },
  }
}

export function createAntigravityUsageAdapter(
  options: UsageAdapterOptions<AntigravityUsage> = {},
): DashboardServiceAdapter {
  const readServices = async (signal?: AbortSignal) => {
    const usage = await (options.getUsage ?? getAntigravityUsage)(signal)
    if (usage.status === 'unavailable') return []
    return serviceFromAntigravityUsage(usage)
  }
  return {
    id: 'antigravity',
    async getService(signal?: AbortSignal) {
      return (await readServices(signal))[0] ?? null
    },
    async getServices(signal?: AbortSignal) {
      return readServices(signal)
    },
  }
}

export function createUsageAdapters(): DashboardServiceAdapter[] {
  const adapters = [
    createClaudeUsageAdapter(),
    createCodexUsageAdapter(),
    createAntigravityUsageAdapter(),
  ]
  return adapters
}
