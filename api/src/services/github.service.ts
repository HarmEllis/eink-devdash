import type { DashboardService, DashboardServiceAdapter } from './dashboard-service.js'

const BASE = 'https://api.github.com'
const CACHE_TTL_MS = 60_000
const NOTIFICATION_PAGE_SIZE = 100
const MAX_NOTIFICATION_PAGES = 50

export type GitHubStats = {
  issues: number
  pullRequests: number
  prs: number
  dependabot: number
  securityAlerts: number
  notifications: number | null
  authError: boolean
  error: boolean
}

type GitHubFetch = (url: string, init?: RequestInit) => Promise<Response>

type GitHubServiceAdapterOptions = {
  token?: string
  notificationsToken?: string
  fetch?: GitHubFetch
}

class GitHubHttpError extends Error {
  constructor(
    readonly status: number,
    readonly path: string,
    message: string,
  ) {
    super(`GitHub ${status}: ${path}${message ? `: ${message}` : ''}`)
  }
}

function isAuthError(err: unknown): boolean {
  return err instanceof GitHubHttpError && (err.status === 401 || err.status === 403)
}

async function responseErrorMessage(res: Response): Promise<string> {
  try {
    const body = await res.text()
    if (!body) return ''
    const parsed = JSON.parse(body) as { message?: unknown }
    return typeof parsed.message === 'string' ? parsed.message : body.slice(0, 120)
  } catch {
    return ''
  }
}

async function ghFetchJson(path: string, token: string, fetchImpl: GitHubFetch): Promise<any> {
  const res = await fetchImpl(`${BASE}${path}`, {
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
    },
  })
  if (!res.ok) throw new GitHubHttpError(res.status, path, await responseErrorMessage(res))
  return res.json() as Promise<any>
}

async function ghFetchArray(path: string, token: string, fetchImpl: GitHubFetch): Promise<any[]> {
  const data = await ghFetchJson(path, token, fetchImpl)
  return Array.isArray(data) ? data : []
}

function nextPathFromLinkHeader(link: string | null): string | null {
  if (!link) return null
  for (const part of link.split(',')) {
    if (!part.includes('rel="next"')) continue
    const match = part.match(/^\s*<([^>]+)>/)
    if (!match) continue
    try {
      const url = new URL(match[1])
      if (url.origin !== BASE) return null
      return `${url.pathname}${url.search}`
    } catch {
      return null
    }
  }
  return null
}

async function countUnreadNotifications(token: string, fetchImpl: GitHubFetch): Promise<number> {
  let path: string | null = `/notifications?all=false&participating=false&per_page=${NOTIFICATION_PAGE_SIZE}`
  let count = 0

  for (let page = 0; path && page < MAX_NOTIFICATION_PAGES; page++) {
    const res = await fetchImpl(`${BASE}${path}`, {
      headers: {
        Authorization: `Bearer ${token}`,
        Accept: 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
      },
    })
    if (!res.ok) throw new GitHubHttpError(res.status, path, await responseErrorMessage(res))

    const data = await res.json() as unknown
    if (!Array.isArray(data)) {
      throw new Error(`GitHub notifications response was not an array: ${path}`)
    }
    count += data.length
    path = nextPathFromLinkHeader(res.headers.get('link'))
  }

  if (path) {
    console.warn(`[github] notification pagination stopped after ${MAX_NOTIFICATION_PAGES} pages`)
  }
  return count
}

async function fetchNotificationCount(
  token: string,
  fetchImpl: GitHubFetch,
): Promise<number | null> {
  try {
    return await countUnreadNotifications(token, fetchImpl)
  } catch (err) {
    if (err instanceof GitHubHttpError) {
      console.warn(
        '[github] unread notifications unavailable:',
        err.message,
      )
      return null
    }
    throw err
  }
}

async function fetchRepoArray(
  path: string,
  token: string,
  fetchImpl: GitHubFetch,
  options: { allowForbidden?: boolean } = {},
): Promise<any[]> {
  try {
    return await ghFetchArray(path, token, fetchImpl)
  } catch (err) {
    if (err instanceof GitHubHttpError) {
      if (err.status === 404 || (options.allowForbidden && err.status === 403)) {
        console.warn(`[github] skipped ${path}: ${err.status}`)
        return []
      }
      if (isAuthError(err)) throw err
      console.warn(`[github] skipped ${path}: ${err.message}`)
      return []
    }
    throw err
  }
}

async function fetchFresh(
  token: string,
  notificationsToken: string | undefined,
  fetchImpl: GitHubFetch,
): Promise<GitHubStats> {
  const repos: any[] = await ghFetchArray('/user/repos?per_page=100&type=owner', token, fetchImpl)
  const notificationsPromise = notificationsToken?.trim()
    ? fetchNotificationCount(notificationsToken, fetchImpl)
    : Promise.resolve(null)

  const limit = 5
  let issues = 0, pullRequests = 0, dependabot = 0

  for (let i = 0; i < repos.length; i += limit) {
    const batch = repos.slice(i, i + limit)
    await Promise.all(batch.map(async (repo) => {
      const owner = repo.owner.login
      const name = repo.name

      const [issueList, prList, alertList] = await Promise.all([
        fetchRepoArray(
          `/repos/${owner}/${name}/issues?state=open&per_page=100`,
          token,
          fetchImpl,
          { allowForbidden: true },
        ),
        fetchRepoArray(
          `/repos/${owner}/${name}/pulls?state=open&per_page=100`,
          token,
          fetchImpl,
          { allowForbidden: true },
        ),
        fetchRepoArray(
          `/repos/${owner}/${name}/dependabot/alerts?state=open&per_page=100`,
          token,
          fetchImpl,
          { allowForbidden: true },
        ),
      ])

      const prNumbers = new Set((prList as any[]).map((p: any) => p.number))
      issues += (issueList as any[]).filter((i: any) => !prNumbers.has(i.number)).length
      pullRequests += prList.length
      dependabot += alertList.length
    }))
  }

  const notifications = await notificationsPromise

  return {
    issues,
    pullRequests,
    prs: pullRequests,
    dependabot,
    securityAlerts: dependabot,
    notifications,
    authError: false,
    error: false,
  }
}

export function serviceFromGitHubStats(stats: GitHubStats): DashboardService {
  const counters = [
    { id: 'issues', label: 'Issues', value: stats.issues },
    { id: 'pullRequests', label: 'Pulls', value: stats.pullRequests },
    {
      id: 'securityAlerts',
      label: 'Security',
      value: stats.securityAlerts,
      ...(stats.securityAlerts > 0 ? { alert: true } : {}),
    },
  ]
  if (stats.notifications !== null) {
    counters.push({ id: 'notifications', label: 'Unread', value: stats.notifications })
  }

  return {
    id: 'github',
    kind: 'code-host',
    provider: 'github',
    label: 'GitHub',
    status: stats.authError ? 'auth_error' : stats.error ? 'error' : 'ok',
    counters,
  }
}

export function createGitHubServiceAdapter(
  options: GitHubServiceAdapterOptions = {},
): DashboardServiceAdapter {
  const fetchImpl = options.fetch ?? fetch
  let cache: { data: GitHubStats; ts: number } | null = null

  return {
    id: 'github',
    async getService() {
      const token = options.token ?? process.env.GITHUB_TOKEN
      if (!token?.trim()) return null
      const notificationsToken = options.notificationsToken ?? process.env.GITHUB_NOTIFICATIONS_TOKEN
      if (cache && Date.now() - cache.ts < CACHE_TTL_MS) {
        return serviceFromGitHubStats(cache.data)
      }

      try {
        const data = await fetchFresh(token, notificationsToken, fetchImpl)
        cache = { data, ts: Date.now() }
        return serviceFromGitHubStats(data)
      } catch (err) {
        console.error('GitHub fetch error:', err)
        const authError = isAuthError(err)
        const data = cache
          ? { ...cache.data, authError, error: !authError }
          : {
              issues: 0,
              pullRequests: 0,
              prs: 0,
              dependabot: 0,
              securityAlerts: 0,
              notifications: null,
              authError,
              error: !authError,
            }
        return serviceFromGitHubStats(data)
      }
    },
  }
}

const defaultGitHubServiceAdapter = createGitHubServiceAdapter()

export async function getGitHubStats(): Promise<GitHubStats | null> {
  const service = await defaultGitHubServiceAdapter.getService()
  if (!service) return null

  const counter = (id: string) => service.counters?.find((item) => item.id === id)?.value ?? 0
  return {
    issues: counter('issues'),
    pullRequests: counter('pullRequests'),
    prs: counter('pullRequests'),
    dependabot: counter('securityAlerts'),
    securityAlerts: counter('securityAlerts'),
    notifications: counter('notifications'),
    authError: service.status === 'auth_error',
    error: service.status === 'error',
  }
}
