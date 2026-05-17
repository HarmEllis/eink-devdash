import { readdir, readFile } from 'fs/promises'
import { homedir } from 'os'
import { join } from 'path'

type CodexUsage = { dailyUsed: number; dailyLimit: number }

const OPENAI_API_KEY = process.env.OPENAI_API_KEY
const CODEX_DIR = join(homedir(), '.codex')

async function fetchFromApi(): Promise<CodexUsage | null> {
  if (!OPENAI_API_KEY) return null

  /* OpenAI usage endpoint expects start_time as Unix seconds (UTC). */
  const startOfDay = new Date()
  startOfDay.setUTCHours(0, 0, 0, 0)
  const startTime = Math.floor(startOfDay.getTime() / 1000)
  const url = `https://api.openai.com/v1/organization/usage/completions?start_time=${startTime}&limit=1`

  try {
    const res = await fetch(url, {
      headers: { Authorization: `Bearer ${OPENAI_API_KEY}` },
    })
    if (!res.ok) {
      console.warn(`[codex] usage API ${res.status}, falling back to log parsing`)
      return null
    }
    const json = await res.json() as any
    const used = json?.data?.[0]?.num_model_requests ?? 0
    return { dailyUsed: used, dailyLimit: 0 }
  } catch (err) {
    console.warn('[codex] usage API failed, falling back to log parsing', err)
    return null
  }
}

async function parseFromLogs(): Promise<CodexUsage> {
  try {
    const files = await readdir(CODEX_DIR)
    const today = new Date().toISOString().split('T')[0]
    let count = 0

    for (const file of files) {
      if (!file.endsWith('.json') && !file.endsWith('.log')) continue
      try {
        const content = await readFile(join(CODEX_DIR, file), 'utf8')
        const lines = content.split('\n').filter(Boolean)
        for (const line of lines) {
          if (line.includes(today) && line.includes('completion')) count++
        }
      } catch { /* skip unreadable files */ }
    }

    return { dailyUsed: count, dailyLimit: 0 }
  } catch {
    return { dailyUsed: 0, dailyLimit: 0 }
  }
}

export async function getCodexUsage(): Promise<CodexUsage> {
  const apiResult = await fetchFromApi()
  if (apiResult) return apiResult
  return parseFromLogs()
}
