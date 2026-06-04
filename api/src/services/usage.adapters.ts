import type { DashboardService, DashboardServiceAdapter } from './dashboard-service.js'
import { getClaudeUsage } from './claude.service.js'
import { getCodexUsage } from './codex.service.js'

type ClaudeRateLimit = {
  used: number
  limit: number
  resetInSeconds: number
}

type ClaudeUsage = {
  fiveHour: ClaudeRateLimit
  weekly: ClaudeRateLimit
  authError: boolean
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
}

type UsageAdapterOptions<TUsage> = {
  getUsage?: () => Promise<TUsage>
}

export function serviceFromClaudeUsage(usage: ClaudeUsage): DashboardService {
  return {
    id: 'claude',
    kind: 'usage',
    provider: 'claude',
    label: 'Claude',
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
}

export function serviceFromCodexUsage(usage: CodexUsage): DashboardService {
  return {
    id: 'codex',
    kind: 'usage',
    provider: 'codex',
    label: 'Codex',
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
}

export function createClaudeUsageAdapter(
  options: UsageAdapterOptions<ClaudeUsage> = {},
): DashboardServiceAdapter {
  return {
    id: 'claude',
    async getService() {
      const usage = await (options.getUsage ?? getClaudeUsage)()
      return serviceFromClaudeUsage(usage)
    },
  }
}

export function createCodexUsageAdapter(
  options: UsageAdapterOptions<CodexUsage> = {},
): DashboardServiceAdapter {
  return {
    id: 'codex',
    async getService() {
      const usage = await (options.getUsage ?? getCodexUsage)()
      return serviceFromCodexUsage(usage)
    },
  }
}

export function createUsageAdapters(): DashboardServiceAdapter[] {
  return [
    createClaudeUsageAdapter(),
    createCodexUsageAdapter(),
  ]
}
