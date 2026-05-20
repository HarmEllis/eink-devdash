import { readFile } from 'fs/promises'
import { homedir } from 'os'
import { join } from 'path'

type RateLimit = { used: number; limit: number; resetInSeconds: number }
type ClaudeUsage = {
  fiveHour: RateLimit
  weekly: RateLimit
  authError: boolean
}

const CREDENTIALS_PATH = join(homedir(), '.claude', '.credentials.json')

async function readOAuthToken(): Promise<string | null> {
  try {
    const raw = await readFile(CREDENTIALS_PATH, 'utf8')
    const creds = JSON.parse(raw)
    const token = creds?.claudeAiOauth?.accessToken
    if (!token) return null

    const expiresAt = creds?.claudeAiOauth?.expiresAt
    if (expiresAt && Date.now() > expiresAt) return null

    return token as string
  } catch {
    return null
  }
}

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

export async function getClaudeUsage(): Promise<ClaudeUsage> {
  const empty: ClaudeUsage = {
    fiveHour: { used: 0, limit: 0, resetInSeconds: 0 },
    weekly: { used: 0, limit: 0, resetInSeconds: 0 },
    authError: true,
  }

  const token = await readOAuthToken()
  if (!token) return empty

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
    })

    if (res.status === 401) return empty
    if (!res.ok) {
      console.warn(`[claude] usage probe HTTP ${res.status}`)
      return empty
    }

    const fiveHour = parseRateLimit(res.headers, '5h')
    const weekly = parseRateLimit(res.headers, '7d')

    if (!fiveHour.present && !weekly.present) {
      // 200 OK but no unified headers — token authenticates but the rate-
      // limit surface is unavailable for this account. Surface as an
      // explicit error rather than silently rendering 0% bars.
      console.warn('[claude] unified ratelimit headers missing on 200 response')
      return empty
    }

    return {
      fiveHour: fiveHour.rate,
      weekly: weekly.rate,
      authError: false,
    }
  } catch (err) {
    console.warn('[claude] usage probe failed', err)
    return empty
  }
}
