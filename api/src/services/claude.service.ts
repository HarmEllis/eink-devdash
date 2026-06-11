import { ClaudeCredentialStore } from './claude-credentials.js'

type RateLimit = { used: number; limit: number; resetInSeconds: number }

/* Extra-usage ("usage credits") spend. `amount`/`limit` are major currency
 * units (e.g. EUR 0.91 / 17.00). `percent` is the share of the monthly cap
 * consumed (0..100) that drives the firmware bar; `null` means "no percent
 * source" (env override) so the device falls back to an amount-capped bar. */
export type ExtraUsage = {
  amount: number
  percent: number | null
  limit: number | null
  currency: string
}

type ClaudeUsage = {
  fiveHour: RateLimit
  weekly: RateLimit
  authError: boolean
  extraUsage?: ExtraUsage | null
}

const credentialStore = new ClaudeCredentialStore()
export const CLAUDE_ADAPTER_BUDGET_MS = 11_000
const PROBE_TIMEOUT_MS = 5_000
const USAGE_TIMEOUT_MS = 5_000
const OAUTH_USAGE_ENDPOINT = 'https://api.anthropic.com/api/oauth/usage'

/* Minor-unit exponent per ISO-4217 code. Only currencies we can both convert
 * correctly AND render a symbol for are supported; anything else is omitted
 * rather than guessed (a wrong exponent would misreport money). */
const CURRENCY_EXPONENTS: Record<string, number> = { EUR: 2, USD: 2 }

function minorToMajor(minor: number, currency: string): number | null {
  const exponent = CURRENCY_EXPONENTS[currency]
  if (exponent === undefined) return null
  return minor / 10 ** exponent
}

/* Operator-supplied overage spend in USD. An explicit override that wins over
 * the live read (precedence: override > live > absent) — useful when the
 * undocumented usage endpoint drifts. Only a value > 0 is honoured. */
function parseOverageUsd(raw: string | undefined): number | null {
  if (!raw) return null
  const n = Number.parseFloat(raw.trim())
  return Number.isFinite(n) && n > 0 ? n : null
}
const CLAUDE_OVERAGE_USD = parseOverageUsd(process.env.CLAUDE_OVERAGE_USD)

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

function resetFromIso(value: unknown): number {
  if (typeof value !== 'string') return 0
  const ms = Date.parse(value)
  return Number.isFinite(ms) ? Math.max(0, Math.round((ms - Date.now()) / 1000)) : 0
}

function windowFromUsage(body: Record<string, unknown>, key: string): ParsedRateLimit {
  // The /api/oauth/usage body expresses utilization as a 0..100 percentage
  // (unlike the 0..1 fraction in the probe headers) plus an ISO reset.
  const window = body[key] as Record<string, unknown> | undefined
  const util = window?.utilization
  const present = typeof util === 'number' && Number.isFinite(util)
  const used = present ? Math.max(0, Math.min(100, Math.round(util as number))) : 0
  return {
    rate: { used, limit: present ? 100 : 0, resetInSeconds: resetFromIso(window?.resets_at) },
    present,
  }
}

function extraUsageFromBody(body: Record<string, unknown>): ExtraUsage | null {
  const e = body.extra_usage as Record<string, unknown> | undefined
  if (!e || e.is_enabled !== true) return null

  const usedMinor = e.used_credits
  if (typeof usedMinor !== 'number' || !Number.isFinite(usedMinor) || usedMinor <= 0) return null

  const currency = typeof e.currency === 'string' ? e.currency.toUpperCase() : ''
  const amount = minorToMajor(usedMinor, currency)
  if (amount === null) {
    console.warn(`[claude] extra_usage currency "${currency || '?'}" unsupported; metric omitted`)
    return null
  }

  const limitMinor = e.monthly_limit
  const limit =
    typeof limitMinor === 'number' && Number.isFinite(limitMinor) && limitMinor > 0
      ? minorToMajor(limitMinor, currency)
      : null

  const util = e.utilization
  const percent =
    typeof util === 'number' && Number.isFinite(util)
      ? Math.max(0, Math.min(100, Math.round(util)))
      : limit && limit > 0
        ? Math.max(0, Math.min(100, Math.round((amount / limit) * 100)))
        : null

  return { amount, percent, limit, currency }
}

type OAuthUsageResult =
  | { kind: 'ok'; fiveHour: ParsedRateLimit; weekly: ParsedRateLimit; extraUsage: ExtraUsage | null }
  | { kind: 'unauthorized' }
  | { kind: 'unavailable' }

async function fetchOAuthUsage(
  token: string,
  signal: AbortSignal | undefined,
  deadline: number,
): Promise<OAuthUsageResult> {
  const remaining = deadline - Date.now()
  if (remaining <= 0) return { kind: 'unavailable' }
  const timeout = AbortSignal.timeout(Math.max(1, Math.min(USAGE_TIMEOUT_MS, remaining)))
  const combined = signal ? AbortSignal.any([signal, timeout]) : timeout
  try {
    const res = await fetch(OAUTH_USAGE_ENDPOINT, {
      method: 'GET',
      headers: {
        Authorization: `Bearer ${token}`,
        'anthropic-version': '2023-06-01',
        'anthropic-beta': 'oauth-2025-04-20',
      },
      signal: combined,
    })

    if (res.status === 401) return { kind: 'unauthorized' }
    if (!res.ok) {
      console.warn(`[claude] oauth usage HTTP ${res.status}`)
      return { kind: 'unavailable' }
    }

    const body = (await res.json()) as Record<string, unknown>
    return {
      kind: 'ok',
      fiveHour: windowFromUsage(body, 'five_hour'),
      weekly: windowFromUsage(body, 'seven_day'),
      extraUsage: extraUsageFromBody(body),
    }
  } catch (err) {
    if (signal?.aborted) throw signal.reason ?? err
    console.warn('[claude] oauth usage read failed', err instanceof Error ? err.message : err)
    return { kind: 'unavailable' }
  }
}

// Billed fallback: the original POST /v1/messages probe (+ one 401 refresh
// retry). Used only when the free GET cannot supply window data.
async function probeForWindows(
  token: string,
  signal: AbortSignal | undefined,
  deadline: number,
): Promise<ClaudeUsage> {
  const first = await probeUsage(token, signal, deadline)
  if (first.kind === 'usage') return first.usage
  if (first.kind === 'empty') return EMPTY

  credentialStore.invalidateCache()
  const refreshed = await credentialStore.getAccessToken({ forceRefresh: true, signal, deadline })
  if (!refreshed || refreshed === token) return EMPTY

  const second = await probeUsage(refreshed, signal, deadline)
  return second.kind === 'usage' ? second.usage : EMPTY
}

async function resolveClaudeUsage(signal?: AbortSignal): Promise<ClaudeUsage> {
  const deadline = Date.now() + CLAUDE_ADAPTER_BUDGET_MS
  let token = await credentialStore.getAccessToken({ signal, deadline })
  if (!token) return EMPTY

  // Primary: free, read-only GET. Also yields 5h/7d windows, so the billed
  // probe is only needed when the GET cannot provide them.
  let got = await fetchOAuthUsage(token, signal, deadline)
  if (got.kind === 'unauthorized') {
    credentialStore.invalidateCache()
    const refreshed = await credentialStore.getAccessToken({ forceRefresh: true, signal, deadline })
    if (refreshed && refreshed !== token) {
      token = refreshed
      got = await fetchOAuthUsage(token, signal, deadline)
    }
  }

  if (got.kind === 'ok') {
    const windowsOk = got.fiveHour.present || got.weekly.present
    if (windowsOk) {
      return {
        fiveHour: got.fiveHour.rate,
        weekly: got.weekly.rate,
        authError: false,
        extraUsage: got.extraUsage,
      }
    }
    // Parsed but no usable windows: get windows from the probe, but preserve
    // the valid spend the GET already returned rather than discarding it.
    const probed = await probeForWindows(token, signal, deadline)
    return { ...probed, extraUsage: got.extraUsage ?? probed.extraUsage }
  }

  // GET unavailable (or still unauthorized after refresh): probe for windows
  // only; no spend source in this path.
  return probeForWindows(token, signal, deadline)
}

export async function getClaudeUsage(signal?: AbortSignal): Promise<ClaudeUsage> {
  const usage = await resolveClaudeUsage(signal)
  // Precedence: an explicit env override wins over the live read. It carries an
  // amount only (USD, no percent/limit), so the device shows an amount-capped bar.
  if (CLAUDE_OVERAGE_USD != null) {
    return {
      ...usage,
      extraUsage: { amount: CLAUDE_OVERAGE_USD, percent: null, limit: null, currency: 'USD' },
    }
  }
  return usage
}
