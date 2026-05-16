import { readdir, readFile } from 'fs/promises'
import { homedir } from 'os'
import { join } from 'path'

type CodexUsage = { daily: { used: number; limit: number } }

const OPENAI_API_KEY = process.env.OPENAI_API_KEY
const CODEX_DIR = join(homedir(), '.codex')

async function fetchFromApi(): Promise<CodexUsage | null> {
  if (!OPENAI_API_KEY) return null

  const today = new Date().toISOString().split('T')[0]
  const url = `https://api.openai.com/v1/organization/usage/completions?start_time=${today}&limit=1`

  try {
    const res = await fetch(url, {
      headers: { Authorization: `Bearer ${OPENAI_API_KEY}` },
    })
    if (!res.ok) return null
    const json = await res.json() as any
    const used = json?.data?.[0]?.num_model_requests ?? 0
    return { daily: { used, limit: 0 } }
  } catch {
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

    return { daily: { used: count, limit: 0 } }
  } catch {
    return { daily: { used: 0, limit: 0 } }
  }
}

export async function getCodexUsage(): Promise<CodexUsage> {
  const apiResult = await fetchFromApi()
  if (apiResult) return apiResult
  return parseFromLogs()
}
