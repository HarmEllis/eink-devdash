import { open, readdir, stat } from 'fs/promises'
import { homedir } from 'os'
import { join } from 'path'

type CodexSource = 'chatgpt' | 'api-key'
type CodexLimitReached = 'short' | 'long' | null

type CodexWindow = {
  usedPercent: number
  label: string
  resetsAt: number | null
  resetInSeconds: number
}

type CodexUsage = {
  source: CodexSource
  planType: string | null
  short: CodexWindow
  long: CodexWindow
  reachedLimit: CodexLimitReached
}

type RateLimitWindow = {
  used_percent?: unknown
  resets_at?: unknown
}

type RateLimits = {
  primary?: RateLimitWindow
  secondary?: RateLimitWindow
  plan_type?: unknown
  rate_limit_reached_type?: unknown
}

const CODEX_SESSIONS_DIR = join(homedir(), '.codex', 'sessions')
const READ_CHUNK_BYTES = 64 * 1024

function emptyChatGptUsage(): CodexUsage {
  return {
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

function windowFromRateLimit(window: RateLimitWindow | undefined, label: string): CodexWindow {
  const usedPercent = numberOrNull(window?.used_percent) ?? 0
  const resetsAt = numberOrNull(window?.resets_at)
  const resetInSeconds = resetsAt !== null
    ? Math.max(0, Math.round(resetsAt - Date.now() / 1000))
    : 0
  return { usedPercent, label, resetsAt, resetInSeconds }
}

function normalizeReachedLimit(value: unknown): CodexLimitReached {
  if (value === 'primary' || value === 'short') return 'short'
  if (value === 'secondary' || value === 'long') return 'long'
  return null
}

function usageFromRateLimits(rateLimits: RateLimits): CodexUsage {
  return {
    source: 'chatgpt',
    planType: typeof rateLimits.plan_type === 'string' ? rateLimits.plan_type : null,
    short: windowFromRateLimit(rateLimits.primary, '5h'),
    long: windowFromRateLimit(rateLimits.secondary, '7d'),
    reachedLimit: normalizeReachedLimit(rateLimits.rate_limit_reached_type),
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
  try {
    for (const filePath of await listNewestSessionFiles()) {
      const rateLimits = await findLastRateLimits(filePath)
      if (rateLimits) return usageFromRateLimits(rateLimits)
    }
  } catch (err) {
    console.warn('[codex] failed to read ChatGPT session usage', err)
  }
  return emptyChatGptUsage()
}

export async function getCodexUsage(): Promise<CodexUsage> {
  /* v1 supports the ChatGPT-auth Codex CLI path only. A future api-key adapter
   * can auto-select when OPENAI_API_KEY is present and map spend budgets onto
   * the same short/long wire shape. */
  return getChatGptUsage()
}
