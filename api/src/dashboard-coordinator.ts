import {
  buildDashboardPayload,
  type DashboardPayload,
} from './routes/dashboard.js'
import type { DashboardServiceAdapter } from './services/dashboard-service.js'

export const DASHBOARD_BUILD_BUDGET_MS = 12_000
export const ABORT_GRACE_MS = 5_000

export type DashboardCoordinator = {
  getDashboard(signal?: AbortSignal): Promise<DashboardPayload>
}

type CoordinatorOptions = {
  budgetMs?: number
  abortGraceMs?: number
  now?: () => Date
  build?: typeof buildDashboardPayload
  onUnrecoverable?: () => void
}

function callerAbort<T>(promise: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (!signal) return promise
  if (signal.aborted) return Promise.reject(signal.reason ?? new Error('Aborted'))
  return new Promise<T>((resolve, reject) => {
    let settled = false
    const finish = (handler: (value: any) => void, value: unknown) => {
      if (settled) return
      settled = true
      signal.removeEventListener('abort', onAbort)
      handler(value)
    }
    const onAbort = () => finish(reject, signal.reason ?? new Error('Aborted'))
    signal.addEventListener('abort', onAbort, { once: true })
    void promise.then(
      (value) => finish(resolve, value),
      (error) => finish(reject, error),
    )
  })
}

export function createDashboardCoordinator(
  adapters: DashboardServiceAdapter[],
  timeZone?: string,
  options: CoordinatorOptions = {},
): DashboardCoordinator {
  const budgetMs = options.budgetMs ?? DASHBOARD_BUILD_BUDGET_MS
  const abortGraceMs = options.abortGraceMs ?? ABORT_GRACE_MS
  const buildDashboard = options.build ?? buildDashboardPayload
  const onUnrecoverable = options.onUnrecoverable ?? (() => process.exit(1))
  let currentBuild: Promise<DashboardPayload> | null = null
  let sharedGuarded: Promise<DashboardPayload> | null = null

  function beginBuild(): Promise<DashboardPayload> {
    const internal = new AbortController()
    let settled = false
    let graceTimer: ReturnType<typeof setTimeout> | null = null
    let rejectBudget!: (reason: Error) => void
    const budgetFailure = new Promise<never>((_resolve, reject) => { rejectBudget = reject })
    const build = buildDashboard(
      (options.now ?? (() => new Date()))(),
      adapters,
      timeZone,
      { signal: internal.signal },
    )
    currentBuild = build
    const budgetTimer = setTimeout(() => {
      internal.abort(new Error('Dashboard build budget exceeded'))
      rejectBudget(new Error(`Dashboard build exceeded ${budgetMs}ms budget`))
      graceTimer = setTimeout(() => {
        if (currentBuild === build && !settled) onUnrecoverable()
      }, abortGraceMs)
    }, budgetMs)
    const onSettle = () => {
      settled = true
      clearTimeout(budgetTimer)
      if (graceTimer) clearTimeout(graceTimer)
      if (currentBuild === build) {
        currentBuild = null
        sharedGuarded = null
      }
    }
    void build.then(onSettle, onSettle)
    sharedGuarded = Promise.race([build, budgetFailure])
    return sharedGuarded
  }

  return {
    getDashboard(signal?: AbortSignal) {
      const guarded = currentBuild ? sharedGuarded! : beginBuild()
      return callerAbort(guarded, signal)
    },
  }
}
