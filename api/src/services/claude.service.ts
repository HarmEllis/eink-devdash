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

function parseRateLimit(headers: Headers, prefix: string): RateLimit {
  const used = parseInt(headers.get(`anthropic-ratelimit-${prefix}-used`) ?? '0')
  const limit = parseInt(headers.get(`anthropic-ratelimit-${prefix}-limit`) ?? '0')
  const resetAt = headers.get(`anthropic-ratelimit-${prefix}-reset`)
  const resetInSeconds = resetAt
    ? Math.max(0, Math.round((new Date(resetAt).getTime() - Date.now()) / 1000))
    : 0
  return { used, limit, resetInSeconds }
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
      },
      body: JSON.stringify({
        model: 'claude-haiku-4-5-20251001',
        max_tokens: 1,
        messages: [{ role: 'user', content: '.' }],
      }),
    })

    if (res.status === 401) return empty

    return {
      fiveHour: parseRateLimit(res.headers, 'tokens-5m'),
      weekly: parseRateLimit(res.headers, 'tokens-1w'),
      authError: false,
    }
  } catch {
    return empty
  }
}
