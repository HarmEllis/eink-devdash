import { readFile } from 'node:fs/promises'
import { homedir } from 'node:os'
import { join } from 'node:path'

export type AntigravityStatus = 'ok' | 'unavailable' | 'auth_error' | 'error'

export type AntigravityWindow = {
  usedPercent: number
  label: string
  resetsAt: number | null
  resetInSeconds: number
}

export type AntigravityGroup = {
  id: 'gemini' | 'claudeGpt'
  label: string
  short: AntigravityWindow
  long: AntigravityWindow
  reachedLimit: 'short' | 'long' | null
}

export type AntigravityUsage = {
  status: AntigravityStatus
  short: AntigravityWindow
  long: AntigravityWindow
  reachedLimit: 'short' | 'long' | null
  groups: AntigravityGroup[]
  spend?: number | null
}

type StoredCredentials = {
  accessToken: string | null
  refreshToken: string | null
  expiresAt: number | null
}

type QuotaCandidate = {
  remainingFraction: number
  resetTime: string | null
}

const QUOTA_URL =
  'https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary'
const TOKEN_URL = 'https://oauth2.googleapis.com/token'
// Installed-app OAuth client metadata is public and must match Antigravity's
// client so its existing refresh token can be exchanged inside the container.
const GOOGLE_CLIENT_ID =
  '1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com'
const GOOGLE_CLIENT_SECRET = ['GOCSPX', 'K58FWR486LdLJ1mLB8sXC4z6qDAf'].join('-')
const TOKEN_REFRESH_MARGIN_MS = 60_000
const REQUEST_TIMEOUT_MS = 10_000

class AntigravityUnavailableError extends Error {}
class AntigravityAuthError extends Error {}

function emptyWindow(label: string): AntigravityWindow {
  return { label, usedPercent: 0, resetsAt: null, resetInSeconds: 0 }
}

function emptyUsage(status: AntigravityStatus): AntigravityUsage {
  const usage: AntigravityUsage = {
    status,
    reachedLimit: null,
    short: emptyWindow('5h'),
    long: emptyWindow('7d'),
    groups: [],
  }

  const overage = Number.parseFloat(process.env.ANTIGRAVITY_OVERAGE_USD?.trim() ?? '')
  if (Number.isFinite(overage) && overage > 0) usage.spend = overage

  return usage
}

function requestSignal(signal?: AbortSignal): AbortSignal {
  const timeout = AbortSignal.timeout(REQUEST_TIMEOUT_MS)
  return signal ? AbortSignal.any([signal, timeout]) : timeout
}

function tokenPath(): string {
  return (
    process.env.ANTIGRAVITY_TOKEN_PATH?.trim() ||
    join(homedir(), '.gemini', 'antigravity-cli', 'antigravity-oauth-token')
  )
}

function parseExpiry(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value
  if (typeof value !== 'string') return null
  const parsed = Date.parse(value)
  return Number.isFinite(parsed) ? parsed : null
}

async function loadCredentials(): Promise<StoredCredentials> {
  const envToken = process.env.ANTIGRAVITY_TOKEN?.trim()
  if (envToken) {
    return { accessToken: envToken, refreshToken: null, expiresAt: null }
  }

  let raw: string
  try {
    raw = await readFile(tokenPath(), 'utf8')
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') {
      throw new AntigravityUnavailableError('Antigravity credentials were not found')
    }
    throw error
  }

  const parsed = JSON.parse(raw) as {
    token?: {
      access_token?: unknown
      refresh_token?: unknown
      expiry?: unknown
    }
  }

  const accessToken =
    typeof parsed.token?.access_token === 'string' && parsed.token.access_token.trim()
      ? parsed.token.access_token.trim()
      : null
  const refreshToken =
    typeof parsed.token?.refresh_token === 'string' && parsed.token.refresh_token.trim()
      ? parsed.token.refresh_token.trim()
      : null

  if (!accessToken && !refreshToken) {
    throw new AntigravityAuthError('Antigravity credentials do not contain a usable token')
  }

  return {
    accessToken,
    refreshToken,
    expiresAt: parseExpiry(parsed.token?.expiry),
  }
}

async function refreshAccessToken(refreshToken: string, signal?: AbortSignal): Promise<string> {
  const response = await fetch(TOKEN_URL, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: GOOGLE_CLIENT_ID,
      client_secret: GOOGLE_CLIENT_SECRET,
      grant_type: 'refresh_token',
      refresh_token: refreshToken,
    }),
    signal: requestSignal(signal),
  })

  if (response.status === 400 || response.status === 401 || response.status === 403) {
    throw new AntigravityAuthError('Antigravity token refresh was rejected')
  }
  if (!response.ok) {
    throw new Error(`Antigravity token refresh failed with HTTP ${response.status}`)
  }

  const body = (await response.json()) as { access_token?: unknown }
  if (typeof body.access_token !== 'string' || !body.access_token.trim()) {
    throw new Error('Antigravity token refresh returned no access token')
  }
  return body.access_token.trim()
}

async function retrieveQuotaSummary(
  accessToken: string,
  signal?: AbortSignal,
): Promise<unknown> {
  const response = await fetch(QUOTA_URL, {
    method: 'POST',
    headers: {
      authorization: `Bearer ${accessToken}`,
      'content-type': 'application/json',
      'user-agent': `antigravity/hub/2.1.4 ${process.platform}/${process.arch}`,
    },
    body: '{}',
    signal: requestSignal(signal),
  })

  if (response.status === 401 || response.status === 403) {
    throw new AntigravityAuthError('Antigravity quota request was rejected')
  }
  if (!response.ok) {
    throw new Error(`Antigravity quota request failed with HTTP ${response.status}`)
  }

  return response.json()
}

function finiteNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function selectCandidate(current: QuotaCandidate | null, next: QuotaCandidate): QuotaCandidate {
  return !current || next.remainingFraction < current.remainingFraction ? next : current
}

function windowFromCandidate(
  label: string,
  candidate: QuotaCandidate,
  nowMs: number,
): AntigravityWindow {
  const remaining = Math.min(1, Math.max(0, candidate.remainingFraction))
  const usedPercent = Math.round((1 - remaining) * 10_000) / 100
  const resetMs = candidate.resetTime ? Date.parse(candidate.resetTime) : Number.NaN
  const resetsAt = Number.isFinite(resetMs) ? Math.floor(resetMs / 1000) : null
  const resetInSeconds =
    resetsAt == null ? 0 : Math.max(0, resetsAt - Math.floor(nowMs / 1000))

  return { label, usedPercent, resetsAt, resetInSeconds }
}

export function parseAntigravityQuotaSummary(
  payload: unknown,
  nowMs = Date.now(),
): Pick<AntigravityUsage, 'short' | 'long' | 'reachedLimit' | 'groups'> {
  const root =
    payload && typeof payload === 'object' && 'response' in payload
      ? (payload as { response?: unknown }).response
      : payload
  const groups =
    root && typeof root === 'object' && Array.isArray((root as { groups?: unknown }).groups)
      ? (root as { groups: unknown[] }).groups
      : []

  let short: QuotaCandidate | null = null
  let long: QuotaCandidate | null = null
  const groupCandidates = new Map<
    AntigravityGroup['id'],
    { label: string; short: QuotaCandidate | null; long: QuotaCandidate | null }
  >()

  for (const group of groups) {
    if (!group || typeof group !== 'object') continue
    const value = group as { displayName?: unknown; buckets?: unknown }
    const buckets = value.buckets
    if (!Array.isArray(buckets)) continue
    const displayName = typeof value.displayName === 'string' ? value.displayName : ''
    const normalizedName = displayName.toLowerCase()
    const groupId: AntigravityGroup['id'] | null = normalizedName.includes('gemini')
      ? 'gemini'
      : normalizedName.includes('claude') || normalizedName.includes('gpt')
        ? 'claudeGpt'
        : null
    const knownGroup = groupId
      ? (groupCandidates.get(groupId) ?? {
          label: groupId === 'gemini' ? 'Gemini' : 'Claude/GPT',
          short: null,
          long: null,
        })
      : null

    for (const bucket of buckets) {
      if (!bucket || typeof bucket !== 'object') continue
      const value = bucket as {
        window?: unknown
        remainingFraction?: unknown
        resetTime?: unknown
      }
      const remainingFraction = finiteNumber(value.remainingFraction)
      if (remainingFraction == null) continue
      const candidate: QuotaCandidate = {
        remainingFraction,
        resetTime: typeof value.resetTime === 'string' ? value.resetTime : null,
      }

      if (value.window === '5h') {
        short = selectCandidate(short, candidate)
        if (knownGroup) knownGroup.short = selectCandidate(knownGroup.short, candidate)
      }
      if (value.window === 'weekly') {
        long = selectCandidate(long, candidate)
        if (knownGroup) knownGroup.long = selectCandidate(knownGroup.long, candidate)
      }
    }

    if (groupId && knownGroup) groupCandidates.set(groupId, knownGroup)
  }

  if (!short || !long) {
    throw new Error('Antigravity quota response is missing 5h or weekly buckets')
  }

  const shortWindow = windowFromCandidate('5h', short, nowMs)
  const longWindow = windowFromCandidate('7d', long, nowMs)
  const reachedLimit =
    shortWindow.usedPercent >= 100 ? 'short' : longWindow.usedPercent >= 100 ? 'long' : null

  const parsedGroups: AntigravityGroup[] = []
  for (const id of ['gemini', 'claudeGpt'] as const) {
    const group = groupCandidates.get(id)
    if (!group?.short || !group.long) continue
    const groupShort = windowFromCandidate('5h', group.short, nowMs)
    const groupLong = windowFromCandidate('7d', group.long, nowMs)
    parsedGroups.push({
      id,
      label: group.label,
      short: groupShort,
      long: groupLong,
      reachedLimit:
        groupShort.usedPercent >= 100
          ? 'short'
          : groupLong.usedPercent >= 100
            ? 'long'
            : null,
    })
  }

  return { short: shortWindow, long: longWindow, reachedLimit, groups: parsedGroups }
}

export async function getAntigravityUsage(signal?: AbortSignal): Promise<AntigravityUsage> {
  const usage = emptyUsage('unavailable')

  try {
    const credentials = await loadCredentials()
    let accessToken = credentials.accessToken
    let refreshed = false

    if (
      credentials.refreshToken &&
      (!accessToken ||
        (credentials.expiresAt != null &&
          credentials.expiresAt <= Date.now() + TOKEN_REFRESH_MARGIN_MS))
    ) {
      accessToken = await refreshAccessToken(credentials.refreshToken, signal)
      refreshed = true
    }
    if (!accessToken) {
      throw new AntigravityAuthError('Antigravity credentials have no access token')
    }

    let summary: unknown
    try {
      summary = await retrieveQuotaSummary(accessToken, signal)
    } catch (error) {
      if (
        error instanceof AntigravityAuthError &&
        credentials.refreshToken &&
        !refreshed
      ) {
        accessToken = await refreshAccessToken(credentials.refreshToken, signal)
        summary = await retrieveQuotaSummary(accessToken, signal)
      } else {
        throw error
      }
    }

    return {
      ...usage,
      status: 'ok',
      ...parseAntigravityQuotaSummary(summary),
    }
  } catch (error) {
    if (error instanceof AntigravityUnavailableError) return usage
    if (error instanceof AntigravityAuthError) return { ...usage, status: 'auth_error' }
    console.warn('[antigravity] Failed to retrieve quota summary:', error)
    return { ...usage, status: 'error' }
  }
}
