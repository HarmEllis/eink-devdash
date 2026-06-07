import { ClaudeCredentialStore } from './claude-credentials.js'

type RateLimit = { used: number; limit: number; resetInSeconds: number }
type ClaudeUsage = {
  fiveHour: RateLimit
  weekly: RateLimit
  authError: boolean
}

const credentialStore = new ClaudeCredentialStore()
export const CLAUDE_ADAPTER_BUDGET_MS = 11_000
const PROBE_TIMEOUT_MS = 5_000

type ParsedRateLimit = { rate: RateLimit; present: boolean }

function parseRateLimit(headers: Headers, window: '5h' | '7d'): ParsedRateLimit {
  // OAuth (Pro/Max) tokens expose unified rate-limit headers expressed as a
  // utilization fraction (0..1) plus a unix-seconds reset. Map onto a
  // {used, limit} shape with limit=100 so existing firmware percentage math
  // (used*100/limit) yields utilization*100 unchanged. `present` reports
  // whether the headers actually came back, so the caller can flip an
  // explicit unavailable state instead of silently rendering 0%.
  const utilHeader = headers.get(`anthropic-ratelimit-unified-${window}-utilization`)
  const resetHeader = headers.get(`anthropic-ratelimit-unified-${window}-reset`)

  const utilization = utilHeader ? parseFloat(utilHeader) : NaN
  const present = Number.isFinite(utilization)
  const used = present
    ? Math.max(0, Math.min(100, Math.round(utilization * 100)))
    : 0
  const limit = present ? 100 : 0

  const resetUnix = resetHeader ? parseInt(resetHeader, 10) : NaN
  const resetInSeconds = Number.isFinite(resetUnix)
    ? Math.max(0, resetUnix - Math.floor(Date.now() / 1000))
    : 0

  return { rate: { used, limit, resetInSeconds }, present }
}

function parseRetryAfter(headers: Headers): number {
  const header = headers.get('retry-after')
  if (!header) return 0

  const seconds = Number(header)
  if (Number.isFinite(seconds)) return Math.max(0, Math.ceil(seconds))

  const resetMs = Date.parse(header)
  return Number.isFinite(resetMs)
    ? Math.max(0, Math.ceil((resetMs - Date.now()) / 1000))
    : 0
}

function rateLimitedWindow(headers: Headers, window: '5h' | '7d', fallbackReset: number): RateLimit {
  const parsed = parseRateLimit(headers, window)
  if (parsed.present) {
    return {
      ...parsed.rate,
      resetInSeconds: parsed.rate.resetInSeconds || fallbackReset,
    }
  }

  return { used: 100, limit: 100, resetInSeconds: fallbackReset }
}

function rateLimitedUsage(headers: Headers): ClaudeUsage {
  const fallbackReset = parseRetryAfter(headers)
  return {
    fiveHour: rateLimitedWindow(headers, '5h', fallbackReset),
    weekly: rateLimitedWindow(headers, '7d', fallbackReset),
    authError: false,
  }
}

const EMPTY: ClaudeUsage = {
  fiveHour: { used: 0, limit: 0, resetInSeconds: 0 },
  weekly: { used: 0, limit: 0, resetInSeconds: 0 },
  authError: true,
}

type ProbeOutcome =
  | { kind: 'usage'; usage: ClaudeUsage }
  | { kind: 'unauthorized' }
  | { kind: 'empty' }

async function probeUsage(
  token: string,
  signal: AbortSignal | undefined,
  deadline: number,
): Promise<ProbeOutcome> {
  const remaining = deadline - Date.now()
  if (remaining <= 0) return { kind: 'empty' }
  const timeout = AbortSignal.timeout(Math.max(1, Math.min(PROBE_TIMEOUT_MS, remaining)))
  const combined = signal ? AbortSignal.any([signal, timeout]) : timeout
  try {
    const res = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
        'anthropic-version': '2023-06-01',
        // OAuth-issued tokens (Claude Code / Pro / Max) only emit the
        // anthropic-ratelimit-unified-* headers under this beta. Without
        // it, Anthropic may switch back to the standard per-request
        // ratelimit headers, leaving the dashboard empty.
        'anthropic-beta': 'oauth-2025-04-20',
      },
      body: JSON.stringify({
        model: 'claude-haiku-4-5-20251001',
        max_tokens: 1,
        messages: [{ role: 'user', content: '.' }],
      }),
      signal: combined,
    })

    if (res.status === 401) return { kind: 'unauthorized' }
    if (res.status === 429) {
      console.warn('[claude] usage probe rate limited')
      return { kind: 'usage', usage: rateLimitedUsage(res.headers) }
    }
    if (!res.ok) {
      console.warn(`[claude] usage probe HTTP ${res.status}`)
      return { kind: 'empty' }
    }

    const fiveHour = parseRateLimit(res.headers, '5h')
    const weekly = parseRateLimit(res.headers, '7d')

    if (!fiveHour.present && !weekly.present) {
      console.warn('[claude] unified ratelimit headers missing on 200 response')
      return { kind: 'empty' }
    }

    return {
      kind: 'usage',
      usage: { fiveHour: fiveHour.rate, weekly: weekly.rate, authError: false },
    }
  } catch (err) {
    if (signal?.aborted) throw signal.reason ?? err
    console.warn('[claude] usage probe failed', err instanceof Error ? err.message : err)
    return { kind: 'empty' }
  }
}

export async function getClaudeUsage(signal?: AbortSignal): Promise<ClaudeUsage> {
  const deadline = Date.now() + CLAUDE_ADAPTER_BUDGET_MS
  const token = await credentialStore.getAccessToken({ signal, deadline })
  if (!token) return EMPTY

  const first = await probeUsage(token, signal, deadline)
  if (first.kind === 'usage') return first.usage
  if (first.kind === 'empty') return EMPTY

  // 401 path: cached/disk token was rejected. Invalidate the cache, force a
  // fresh refresh against disk, and retry once before surfacing authError.
  credentialStore.invalidateCache()
  const refreshed = await credentialStore.getAccessToken({ forceRefresh: true, signal, deadline })
  if (!refreshed || refreshed === token) return EMPTY

  const second = await probeUsage(refreshed, signal, deadline)
  return second.kind === 'usage' ? second.usage : EMPTY
}
