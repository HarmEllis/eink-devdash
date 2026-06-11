import { constants as fsConstants } from 'fs'
import {
  access,
  chmod,
  chown,
  readFile,
  rename,
  stat,
  unlink,
  writeFile,
} from 'fs/promises'
import { homedir } from 'os'
import { dirname, join } from 'path'
import lockfile from 'proper-lockfile'

const DEFAULT_CREDENTIALS_PATH = join(homedir(), '.claude', '.credentials.json')

const OAUTH_TOKEN_ENDPOINT = 'https://platform.claude.com/v1/oauth/token'
const OAUTH_CLIENT_ID = '9d1c250a-e61b-44d9-88ed-5944d1962f5e'

const HTTP_TIMEOUT_MS = 10_000
const LOCK_STALE_MS = 30_000
const LOCK_RETRY_OPTIONS = {
  retries: 10,
  factor: 1.5,
  minTimeout: 250,
  maxTimeout: 2_000,
  randomize: true,
}
const REFRESH_SKEW_MS = 60_000

type TokenOptions = {
  forceRefresh?: boolean
  signal?: AbortSignal
  deadline?: number
}

function remainingMs(deadline?: number): number {
  return deadline === undefined ? HTTP_TIMEOUT_MS : Math.max(0, deadline - Date.now())
}

function lockRetriesFor(remaining: number) {
  let delay = LOCK_RETRY_OPTIONS.minTimeout
  let total = 0
  let retries = 0
  while (retries < LOCK_RETRY_OPTIONS.retries && total + delay < remaining) {
    total += delay
    retries += 1
    delay = Math.min(LOCK_RETRY_OPTIONS.maxTimeout, delay * LOCK_RETRY_OPTIONS.factor)
  }
  return { ...LOCK_RETRY_OPTIONS, retries }
}

type ClaudeAiOauth = {
  accessToken?: unknown
  refreshToken?: unknown
  expiresAt?: unknown
  scopes?: unknown
  [key: string]: unknown
}

type CredentialsFile = {
  claudeAiOauth?: ClaudeAiOauth
  [key: string]: unknown
}

type DiskCreds = {
  json: CredentialsFile
  accessToken: string
  refreshToken: string
  expiresAt: number
  scopes: string[]
  mtimeMs: number
  uid: number
  gid: number
}

type RefreshResult = {
  accessToken: string
  expiresAt: number
  refreshToken: string | null
}

function asString(value: unknown): string | null {
  return typeof value === 'string' && value.length > 0 ? value : null
}

function asNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function asStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) return []
  return value.filter((item): item is string => typeof item === 'string')
}

function parseDiskCreds(json: CredentialsFile, statResult: {
  mtimeMs: number
  uid: number
  gid: number
}): DiskCreds | null {
  const oauth = json.claudeAiOauth
  if (!oauth || typeof oauth !== 'object') return null
  const accessToken = asString(oauth.accessToken)
  const refreshToken = asString(oauth.refreshToken)
  const expiresAt = asNumber(oauth.expiresAt) ?? 0
  if (!accessToken || !refreshToken) return null
  return {
    json,
    accessToken,
    refreshToken,
    expiresAt,
    scopes: asStringArray(oauth.scopes),
    mtimeMs: statResult.mtimeMs,
    uid: statResult.uid,
    gid: statResult.gid,
  }
}

export class ClaudeCredentialStore {
  private readonly credentialsPath: string
  private cached: { accessToken: string; expiresAt: number } | null = null
  private writableProbed = false
  private writable = false

  constructor(credentialsPath: string = DEFAULT_CREDENTIALS_PATH) {
    this.credentialsPath = credentialsPath
  }

  async getAccessToken(options: TokenOptions = {}): Promise<string | null> {
    try {
      options.signal?.throwIfAborted()
      return await this.acquireToken(options)
    } catch (err) {
      if (options.signal?.aborted) throw options.signal.reason ?? err
      // Last-resort safety net: anything unexpected in the credential
      // store must not propagate into /dashboard. The route still gets
      // EMPTY/authError and the next request can heal.
      console.warn(
        '[claude] credential store threw unexpectedly',
        err instanceof Error ? err.message : err,
      )
      return null
    }
  }

  private async acquireToken(options: TokenOptions): Promise<string | null> {
    const force = options.forceRefresh === true
    if (!force && this.cached && this.cached.expiresAt - Date.now() > REFRESH_SKEW_MS) {
      return this.cached.accessToken
    }

    const disk = await this.readDisk()
    if (!disk) return null

    if (!force && disk.expiresAt - Date.now() > REFRESH_SKEW_MS) {
      this.cached = { accessToken: disk.accessToken, expiresAt: disk.expiresAt }
      return disk.accessToken
    }

    if (!(await this.canWrite())) {
      // Mount is :ro or host perms wrong — fall back to whatever's on disk,
      // even if expired, so the existing probe gets a chance to surface a
      // real authError instead of us swallowing the token silently.
      this.cached = { accessToken: disk.accessToken, expiresAt: disk.expiresAt }
      return disk.accessToken
    }

    return this.refreshAndPersist(disk, options)
  }

  invalidateCache(): void {
    this.cached = null
  }

  private async readDisk(): Promise<DiskCreds | null> {
    try {
      const [raw, statResult] = await Promise.all([
        readFile(this.credentialsPath, 'utf8'),
        stat(this.credentialsPath),
      ])
      const json = JSON.parse(raw) as CredentialsFile
      return parseDiskCreds(json, {
        mtimeMs: statResult.mtimeMs,
        uid: statResult.uid,
        gid: statResult.gid,
      })
    } catch {
      return null
    }
  }

  private async canWrite(): Promise<boolean> {
    if (this.writableProbed) return this.writable
    this.writableProbed = true
    const dir = dirname(this.credentialsPath)
    const probePath = join(dir, `.devdash-write-probe-${process.pid}`)
    try {
      await access(dir, fsConstants.W_OK)
      await writeFile(probePath, '', { mode: 0o600 })
      await unlink(probePath)
      this.writable = true
    } catch (err) {
      console.warn(
        `[claude] cannot write to ${dir}; refresh path disabled (falling back to read-only probe). `
          + `Check host ownership of ~/.claude (container writes as uid ${process.getuid?.() ?? '?'}).`,
        err instanceof Error ? err.message : err,
      )
      this.writable = false
    }
    return this.writable
  }

  private async refreshAndPersist(initial: DiskCreds, options: TokenOptions): Promise<string | null> {
    options.signal?.throwIfAborted()
    const remaining = remainingMs(options.deadline)
    if (remaining <= 0) throw new Error('Claude credential deadline exceeded')
    let release: (() => Promise<void>) | null = null
    try {
      release = await lockfile.lock(this.credentialsPath, {
        stale: LOCK_STALE_MS,
        retries: lockRetriesFor(remaining),
        realpath: false,
      })
    } catch (err) {
      console.warn(
        '[claude] could not acquire credentials lock; using existing on-disk token',
        err instanceof Error ? err.message : err,
      )
      this.cached = { accessToken: initial.accessToken, expiresAt: initial.expiresAt }
      return initial.accessToken
    }

    try {
      options.signal?.throwIfAborted()
      // Another process may have refreshed while we were waiting on the lock.
      const fresh = await this.readDisk()
      const current = fresh ?? initial
      // Honour the "already valid" fast path for the ordinary (clock-driven)
      // refresh. But a forceRefresh means the caller got a 401 on this exact
      // access token, so an unexpired clock is not enough — only skip the
      // network refresh when disk now holds a *different* (peer-refreshed)
      // token, otherwise we would hand back the same rejected credential.
      const peerRefreshed = current.accessToken !== initial.accessToken
      if (
        (options.forceRefresh !== true || peerRefreshed)
        && current.expiresAt - Date.now() > REFRESH_SKEW_MS
      ) {
        this.cached = { accessToken: current.accessToken, expiresAt: current.expiresAt }
        return current.accessToken
      }

      const refreshed = await this.callRefresh(
        current.refreshToken,
        current.scopes,
        options.signal,
        options.deadline,
      )
      if (!refreshed) return null

      // Final TOCTOU narrowing: re-stat right before write. If someone else
      // wrote a *different* still-valid access token in the meantime,
      // prefer that instead of stomping it.
      const latest = await this.readDisk()
      if (
        latest
        && latest.accessToken !== current.accessToken
        && latest.expiresAt - Date.now() > REFRESH_SKEW_MS
      ) {
        this.cached = { accessToken: latest.accessToken, expiresAt: latest.expiresAt }
        return latest.accessToken
      }

      const baseJson = latest?.json ?? current.json
      const baseUid = latest?.uid ?? current.uid
      const baseGid = latest?.gid ?? current.gid

      // Persistence is best-effort: cache the refreshed token regardless,
      // so a flaky filesystem (ENOSPC, permission race, remounted volume)
      // never blanks the dashboard. The next probe will simply re-refresh
      // from disk once the volume is healthy again.
      this.cached = { accessToken: refreshed.accessToken, expiresAt: refreshed.expiresAt }

      try {
        await this.writeSurgical(baseJson, refreshed, {
          refreshTokenFallback: current.refreshToken,
          uid: baseUid,
          gid: baseGid,
        })
      } catch (err) {
        console.warn(
          '[claude] could not persist refreshed credentials; using in-memory token only',
          err instanceof Error ? err.message : err,
        )
      }

      return refreshed.accessToken
    } finally {
      try {
        await release?.()
      } catch {
        // Lock release best-effort; the stale threshold will reclaim it.
      }
    }
  }

  private async callRefresh(
    refreshToken: string,
    scopes: string[],
    signal?: AbortSignal,
    deadline?: number,
  ): Promise<RefreshResult | null> {
    signal?.throwIfAborted()
    const timeoutMs = Math.min(HTTP_TIMEOUT_MS, remainingMs(deadline))
    if (timeoutMs <= 0) throw new Error('Claude refresh deadline exceeded')
    const controller = new AbortController()
    const onAbort = () => controller.abort(signal?.reason)
    signal?.addEventListener('abort', onAbort, { once: true })
    const timer = setTimeout(() => controller.abort(), timeoutMs)
    try {
      const res = await fetch(OAUTH_TOKEN_ENDPOINT, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        signal: controller.signal,
        body: JSON.stringify({
          grant_type: 'refresh_token',
          refresh_token: refreshToken,
          client_id: OAUTH_CLIENT_ID,
          scope: scopes.join(' '),
        }),
      })

      if (!res.ok) {
        console.warn(`[claude] OAuth refresh failed: HTTP ${res.status}`)
        return null
      }

      const data = (await res.json().catch(() => null)) as Record<string, unknown> | null
      if (!data) {
        console.warn('[claude] OAuth refresh: malformed JSON response')
        return null
      }

      const accessToken = asString(data.access_token)
      if (!accessToken) {
        console.warn('[claude] OAuth refresh: missing access_token in response')
        return null
      }

      const expiresIn = asNumber(data.expires_in)
      const expiresAtFromResponse = asNumber(data.expires_at)
      const expiresAt = expiresAtFromResponse
        ?? (expiresIn !== null ? Date.now() + expiresIn * 1000 : Date.now() + 60 * 60 * 1000)

      const newRefresh = asString(data.refresh_token)

      return { accessToken, expiresAt, refreshToken: newRefresh }
    } catch (err) {
      if (signal?.aborted) throw signal.reason ?? err
      const reason = err instanceof Error ? err.name : 'unknown'
      console.warn(`[claude] OAuth refresh threw (${reason})`)
      return null
    } finally {
      clearTimeout(timer)
      signal?.removeEventListener('abort', onAbort)
    }
  }

  private async writeSurgical(
    base: CredentialsFile,
    refreshed: RefreshResult,
    opts: { refreshTokenFallback: string; uid: number; gid: number },
  ): Promise<void> {
    const cloned: CredentialsFile = { ...base }
    const oauth: ClaudeAiOauth = { ...(cloned.claudeAiOauth ?? {}) }
    oauth.accessToken = refreshed.accessToken
    oauth.expiresAt = refreshed.expiresAt
    oauth.refreshToken = refreshed.refreshToken ?? opts.refreshTokenFallback
    cloned.claudeAiOauth = oauth

    const tempPath = `${this.credentialsPath}.tmp-${process.pid}-${Date.now()}`
    await writeFile(tempPath, JSON.stringify(cloned, null, 2), { mode: 0o600 })
    try {
      await chmod(tempPath, 0o600)
    } catch {
      // Best-effort.
    }
    try {
      await chown(tempPath, opts.uid, opts.gid)
    } catch (err) {
      const code = err instanceof Error && 'code' in err ? (err as NodeJS.ErrnoException).code : null
      if (code !== 'EPERM' && code !== 'ENOSYS') {
        console.warn('[claude] chown on refreshed credentials failed', code ?? 'unknown')
      }
    }
    await rename(tempPath, this.credentialsPath)
  }
}
