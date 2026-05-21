import { spawn } from 'child_process'
import { constants as fsConstants } from 'fs'
import { access, open, readdir, stat } from 'fs/promises'
import { homedir } from 'os'
import { join } from 'path'

type CodexSource = 'chatgpt' | 'api-key'
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
  source: CodexSource
  planType: string | null
  short: CodexWindow
  long: CodexWindow
  reachedLimit: CodexLimitReached
}

type RateLimitWindow = {
  used_percent?: unknown
  usedPercent?: unknown
  resets_at?: unknown
  resetsAt?: unknown
}

type RateLimits = {
  primary?: RateLimitWindow | null
  secondary?: RateLimitWindow | null
  plan_type?: unknown
  planType?: unknown
  rate_limit_reached_type?: unknown
  rateLimitReachedType?: unknown
  limit_id?: unknown
  limitId?: unknown
}

type AppServerRateLimitsResponse = {
  rateLimits?: RateLimits
  rateLimitsByLimitId?: Record<string, RateLimits> | null
}

type JsonRpcResponse = {
  id?: unknown
  result?: unknown
  error?: {
    code?: unknown
    message?: unknown
  }
}

const CODEX_SESSIONS_DIR = join(homedir(), '.codex', 'sessions')
const READ_CHUNK_BYTES = 64 * 1024
const CODEX_PLAN_TYPE = process.env.CODEX_PLAN_TYPE?.trim().toLowerCase() || null
const CODEX_LIVE_USAGE_ENABLED = process.env.CODEX_LIVE_USAGE !== 'false'
const CODEX_APP_SERVER_TIMEOUT_MS = Number.parseInt(
  process.env.CODEX_APP_SERVER_TIMEOUT_MS ?? '8000',
  10,
)

function emptyChatGptUsage(status: CodexStatus): CodexUsage {
  return {
    status,
    source: 'chatgpt',
    planType: null,
    short: { usedPercent: 0, label: '5h', resetsAt: null, resetInSeconds: 0 },
    long: { usedPercent: 0, label: '7d', resetsAt: null, resetInSeconds: 0 },
    reachedLimit: null,
  }
}

function numberOrNull(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function firstNumberOrNull(...values: unknown[]): number | null {
  for (const value of values) {
    const num = numberOrNull(value)
    if (num !== null) return num
  }
  return null
}

function planTypeFromRateLimits(rateLimits: RateLimits): string | null {
  if (typeof rateLimits.plan_type === 'string') return rateLimits.plan_type
  if (typeof rateLimits.planType === 'string') return rateLimits.planType
  return null
}

function windowFromRateLimit(
  window: RateLimitWindow | null | undefined,
  label: string,
  nowSeconds = Date.now() / 1000,
): CodexWindow {
  const resetsAt = firstNumberOrNull(window?.resets_at, window?.resetsAt)
  const resetInSeconds = resetsAt !== null
    ? Math.max(0, Math.round(resetsAt - nowSeconds))
    : 0
  const usedPercent = resetsAt !== null && resetsAt <= nowSeconds
    ? 0
    : firstNumberOrNull(window?.used_percent, window?.usedPercent) ?? 0
  return { usedPercent, label, resetsAt, resetInSeconds }
}

function normalizeReachedLimit(value: unknown): CodexLimitReached {
  if (value === 'primary' || value === 'short') return 'short'
  if (value === 'secondary' || value === 'long') return 'long'
  return null
}

function isStaleRateLimitWindow(window: RateLimitWindow | null | undefined, nowSeconds: number): boolean {
  const resetsAt = firstNumberOrNull(window?.resets_at, window?.resetsAt)
  return resetsAt !== null && resetsAt <= nowSeconds
}

function isStaleRateLimits(rateLimits: RateLimits): boolean {
  const nowSeconds = Date.now() / 1000
  return isStaleRateLimitWindow(rateLimits.primary, nowSeconds)
    && isStaleRateLimitWindow(rateLimits.secondary, nowSeconds)
}

function matchesConfiguredPlan(rateLimits: RateLimits): boolean {
  if (!CODEX_PLAN_TYPE) return true
  return planTypeFromRateLimits(rateLimits)?.toLowerCase() === CODEX_PLAN_TYPE
}

function isErrnoException(err: unknown): err is NodeJS.ErrnoException {
  return err instanceof Error && 'code' in err
}

function usageFromRateLimits(rateLimits: RateLimits): CodexUsage {
  const nowSeconds = Date.now() / 1000
  return {
    status: 'ok',
    source: 'chatgpt',
    planType: planTypeFromRateLimits(rateLimits),
    short: windowFromRateLimit(rateLimits.primary, '5h', nowSeconds),
    long: windowFromRateLimit(rateLimits.secondary, '7d', nowSeconds),
    reachedLimit: normalizeReachedLimit(
      rateLimits.rate_limit_reached_type ?? rateLimits.rateLimitReachedType,
    ),
  }
}

async function resolveCodexCommand(): Promise<string> {
  const configured = process.env.CODEX_CLI_PATH?.trim()
  if (configured) return configured

  const binName = process.platform === 'win32' ? 'codex.cmd' : 'codex'
  const localBin = join(process.cwd(), 'node_modules', '.bin', binName)
  try {
    await access(localBin, fsConstants.X_OK)
    return localBin
  } catch {
    return 'codex'
  }
}

function asJsonRpcResponse(value: unknown): JsonRpcResponse | null {
  if (!value || typeof value !== 'object') return null
  return value as JsonRpcResponse
}

async function readRateLimitsFromAppServer(): Promise<AppServerRateLimitsResponse> {
  const command = await resolveCodexCommand()
  const timeoutMs = Number.isFinite(CODEX_APP_SERVER_TIMEOUT_MS)
    ? CODEX_APP_SERVER_TIMEOUT_MS
    : 8000

  return new Promise((resolve, reject) => {
    const child = spawn(command, ['app-server'], {
      stdio: ['pipe', 'pipe', 'pipe'],
    })
    const initId = 1
    const rateLimitsId = 2
    let stdoutBuffer = ''
    let stderrBuffer = ''
    let settled = false

    const finish = (err: Error | null, result?: AppServerRateLimitsResponse) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      child.kill('SIGTERM')
      if (err) reject(err)
      else resolve(result ?? {})
    }

    const timer = setTimeout(() => {
      finish(new Error(`Codex app-server timed out after ${timeoutMs}ms`))
    }, timeoutMs)

    const send = (message: unknown) => {
      try {
        child.stdin.write(`${JSON.stringify(message)}\n`)
      } catch (err) {
        finish(err instanceof Error ? err : new Error(String(err)))
      }
    }

    const handleLine = (line: string) => {
      if (!line.trim()) return
      let parsed: unknown
      try {
        parsed = JSON.parse(line)
      } catch {
        return
      }

      const response = asJsonRpcResponse(parsed)
      if (!response || typeof response.id !== 'number') return

      if (response.id === initId) {
        if (response.error) {
          finish(new Error(String(response.error.message ?? 'Codex app-server initialize failed')))
          return
        }
        send({ id: rateLimitsId, method: 'account/rateLimits/read', params: null })
        return
      }

      if (response.id === rateLimitsId) {
        if (response.error) {
          finish(new Error(String(response.error.message ?? 'Codex rate-limit read failed')))
          return
        }
        finish(null, response.result as AppServerRateLimitsResponse)
      }
    }

    child.stdout.on('data', (chunk: Buffer) => {
      stdoutBuffer += chunk.toString('utf8')
      const lines = stdoutBuffer.split('\n')
      stdoutBuffer = lines.pop() ?? ''
      for (const line of lines) handleLine(line)
    })

    child.stderr.on('data', (chunk: Buffer) => {
      stderrBuffer = `${stderrBuffer}${chunk.toString('utf8')}`.slice(-2000)
    })
    child.stdin.on('error', (err) => finish(err))

    child.on('error', (err) => finish(err))
    child.on('exit', (code) => {
      if (!settled) {
        const detail = stderrBuffer.trim()
        finish(new Error(
          detail
            ? `Codex app-server exited with code ${code}: ${detail}`
            : `Codex app-server exited with code ${code}`,
        ))
      }
    })

    send({
      id: initId,
      method: 'initialize',
      params: {
        clientInfo: { name: 'eink-devdash-api', version: '0.1.0' },
        capabilities: {
          experimentalApi: true,
          optOutNotificationMethods: [
            'account/updated',
            'account/rateLimits/updated',
            'configWarning',
            'remoteControl/status/changed',
          ],
        },
      },
    })
  })
}

function selectRateLimits(response: AppServerRateLimitsResponse): RateLimits | null {
  const byLimitId = response.rateLimitsByLimitId
  const codexLimit = byLimitId?.codex
  if (codexLimit && matchesConfiguredPlan(codexLimit)) return codexLimit

  if (byLimitId) {
    for (const rateLimits of Object.values(byLimitId)) {
      if (matchesConfiguredPlan(rateLimits)) return rateLimits
    }
  }

  const fallback = response.rateLimits
  if (fallback && matchesConfiguredPlan(fallback)) return fallback
  return null
}

async function getLiveChatGptUsage(): Promise<CodexUsage | null> {
  if (!CODEX_LIVE_USAGE_ENABLED) return null

  try {
    const response = await readRateLimitsFromAppServer()
    const rateLimits = selectRateLimits(response)
    return rateLimits ? usageFromRateLimits(rateLimits) : null
  } catch (err) {
    if (isErrnoException(err) && err.code === 'ENOENT') return null
    console.warn('[codex] live usage probe failed; falling back to session files', err)
    return null
  }
}

async function listNewestSessionFiles(): Promise<string[]> {
  const files: Array<{ path: string; mtimeMs: number }> = []

  async function walk(dir: string, depth: number): Promise<void> {
    const entries = await readdir(dir, { withFileTypes: true })
    for (const entry of entries) {
      const fullPath = join(dir, entry.name)
      if (entry.isDirectory()) {
        if (depth < 3) await walk(fullPath, depth + 1)
        continue
      }
      if (!entry.isFile() || !entry.name.startsWith('rollout-') || !entry.name.endsWith('.jsonl')) {
        continue
      }
      const info = await stat(fullPath)
      files.push({ path: fullPath, mtimeMs: info.mtimeMs })
    }
  }

  await walk(CODEX_SESSIONS_DIR, 0)
  return files
    .sort((a, b) => b.mtimeMs - a.mtimeMs)
    .map((file) => file.path)
}

function parseRateLimitsLine(line: string): RateLimits | null {
  try {
    const parsed = JSON.parse(line) as any
    const payload = parsed?.payload
    if (payload?.type !== 'token_count' || !payload.rate_limits) return null
    return payload.rate_limits as RateLimits
  } catch {
    return null
  }
}

async function findLastRateLimits(filePath: string): Promise<RateLimits | null> {
  const handle = await open(filePath, 'r')
  try {
    const info = await handle.stat()
    let position = info.size
    let carry = ''

    while (position > 0) {
      const readSize = Math.min(READ_CHUNK_BYTES, position)
      position -= readSize

      const buffer = Buffer.allocUnsafe(readSize)
      const { bytesRead } = await handle.read(buffer, 0, readSize, position)
      const text = buffer.subarray(0, bytesRead).toString('utf8')
      const lines = `${text}${carry}`.split('\n')
      carry = lines.shift() ?? ''

      for (let i = lines.length - 1; i >= 0; i--) {
        const line = lines[i].trim()
        if (!line.includes('"token_count"') || !line.includes('"rate_limits"')) continue
        const rateLimits = parseRateLimitsLine(line)
        if (rateLimits) return rateLimits
      }
    }

    const firstLine = carry.trim()
    if (firstLine.includes('"token_count"') && firstLine.includes('"rate_limits"')) {
      return parseRateLimitsLine(firstLine)
    }
    return null
  } finally {
    await handle.close()
  }
}

async function getChatGptUsage(): Promise<CodexUsage> {
  const liveUsage = await getLiveChatGptUsage()
  if (liveUsage) return liveUsage

  try {
    for (const filePath of await listNewestSessionFiles()) {
      const rateLimits = await findLastRateLimits(filePath)
      if (rateLimits && matchesConfiguredPlan(rateLimits) && !isStaleRateLimits(rateLimits)) {
        return usageFromRateLimits(rateLimits)
      }
    }
  } catch (err) {
    if (isErrnoException(err) && err.code === 'ENOENT') {
      return emptyChatGptUsage('unavailable')
    }
    console.warn('[codex] failed to read ChatGPT session usage', err)
    return emptyChatGptUsage('error')
  }
  return emptyChatGptUsage('unavailable')
}

export async function getCodexUsage(): Promise<CodexUsage> {
  /* ChatGPT-auth Codex usage is read live through the app-server when
   * available, with local session files as a fallback. A future api-key adapter
   * can auto-select when OPENAI_API_KEY is present and map spend budgets onto
   * the same short/long wire shape. */
  return getChatGptUsage()
}
