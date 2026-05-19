import { readdir, readFile } from 'fs/promises'
import { homedir } from 'os'
import { join } from 'path'

type CodexUsage = {
  dailyUsed: number
  dailyLimit: number
  weeklyUsed: number
  weeklyLimit: number
}

const OPENAI_API_KEY = process.env.OPENAI_API_KEY
const CODEX_DAILY_LIMIT  = parseInt(process.env.CODEX_DAILY_LIMIT  ?? '0', 10) || 0
const CODEX_WEEKLY_LIMIT = parseInt(process.env.CODEX_WEEKLY_LIMIT ?? '0', 10) || 0
const CODEX_DIR = join(homedir(), '.codex')

async function fetchWindowFromApi(startTime: number): Promise<number | null> {
  if (!OPENAI_API_KEY) return null
  /* Usage endpoint returns time-bucketed data; per-bucket request counts
   * live under `bucket.results[].num_model_requests`. We sum across every
   * result of every bucket. The response is paginated; chase next_page
   * until exhausted or until we hit the safety cap below. */
  let total = 0
  let nextPage: string | undefined = undefined
  let safety = 0
  do {
    const params = new URLSearchParams({
      start_time: String(startTime),
      limit: '200',
    })
    if (nextPage) params.set('page', nextPage)
    const url = `https://api.openai.com/v1/organization/usage/completions?${params}`
    try {
      const res = await fetch(url, {
        headers: { Authorization: `Bearer ${OPENAI_API_KEY}` },
      })
      if (!res.ok) {
        console.warn(`[codex] usage API ${res.status} for window starting ${startTime}`)
        return null
      }
      const json = await res.json() as any
      const buckets: any[] = Array.isArray(json?.data) ? json.data : []
      for (const b of buckets) {
        const results: any[] = Array.isArray(b?.results) ? b.results : []
        for (const r of results) {
          total += (r?.num_model_requests ?? 0)
        }
      }
      nextPage = json?.next_page
    } catch (err) {
      console.warn('[codex] usage API failed', err)
      return null
    }
  } while (nextPage && ++safety < 20)
  return total
}

async function fetchFromApi(): Promise<CodexUsage | null> {
  const now = new Date()
  const startOfDay = new Date(now)
  startOfDay.setUTCHours(0, 0, 0, 0)
  const startOfWeek = new Date(startOfDay.getTime() - 6 * 24 * 60 * 60 * 1000)

  const [daily, weekly] = await Promise.all([
    fetchWindowFromApi(Math.floor(startOfDay.getTime() / 1000)),
    fetchWindowFromApi(Math.floor(startOfWeek.getTime() / 1000)),
  ])
  if (daily === null && weekly === null) return null

  return {
    dailyUsed:   daily  ?? 0,
    dailyLimit:  CODEX_DAILY_LIMIT,
    weeklyUsed:  weekly ?? 0,
    weeklyLimit: CODEX_WEEKLY_LIMIT,
  }
}

async function parseFromLogs(): Promise<CodexUsage> {
  try {
    const files = await readdir(CODEX_DIR)
    const today = new Date().toISOString().split('T')[0]
    const weekStart = new Date(Date.now() - 6 * 24 * 60 * 60 * 1000)
      .toISOString().split('T')[0]
    let dailyCount = 0
    let weeklyCount = 0

    for (const file of files) {
      if (!file.endsWith('.json') && !file.endsWith('.log')) continue
      try {
        const content = await readFile(join(CODEX_DIR, file), 'utf8')
        for (const line of content.split('\n')) {
          if (!line.includes('completion')) continue
          if (line.includes(today)) dailyCount++
          /* Cheap "this week" check: ISO date prefix lexicographically
           * compares correctly within the same year. */
          const dateMatch = line.match(/\d{4}-\d{2}-\d{2}/)
          if (dateMatch && dateMatch[0] >= weekStart) weeklyCount++
        }
      } catch { /* skip unreadable files */ }
    }
    return {
      dailyUsed:   dailyCount,
      dailyLimit:  CODEX_DAILY_LIMIT,
      weeklyUsed:  weeklyCount,
      weeklyLimit: CODEX_WEEKLY_LIMIT,
    }
  } catch {
    return {
      dailyUsed: 0, dailyLimit: CODEX_DAILY_LIMIT,
      weeklyUsed: 0, weeklyLimit: CODEX_WEEKLY_LIMIT,
    }
  }
}

export async function getCodexUsage(): Promise<CodexUsage> {
  const apiResult = await fetchFromApi()
  if (apiResult) return apiResult
  return parseFromLogs()
}
