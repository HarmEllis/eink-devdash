import type { DashboardService, DashboardServiceAdapter } from './dashboard-service.js'

const BASE = 'https://api.github.com'
const CACHE_TTL_MS = 60_000

export type GitHubStats = {
  issues: number
  pullRequests: number
  prs: number
  dependabot: number
  securityAlerts: number
  notifications: number
  authError: boolean
}

type GitHubFetch = (url: string, init?: RequestInit) => Promise<Response>

type GitHubServiceAdapterOptions = {
  token?: string
  fetch?: GitHubFetch
}

async function ghFetch(path: string, token: string, fetchImpl: GitHubFetch) {
  const res = await fetchImpl(`${BASE}${path}`, {
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
    },
  })
  if (!res.ok) throw new Error(`GitHub ${res.status}: ${path}`)
  return res.json() as Promise<any>
}

async function fetchFresh(token: string, fetchImpl: GitHubFetch): Promise<GitHubStats> {
  const repos: any[] = await ghFetch('/user/repos?per_page=100&type=owner', token, fetchImpl)
  const notificationsPromise = ghFetch(
    '/notifications?all=false&participating=false&per_page=100',
    token,
    fetchImpl,
  ).catch(() => [])

  const limit = 5
  let issues = 0, pullRequests = 0, dependabot = 0

  for (let i = 0; i < repos.length; i += limit) {
    const batch = repos.slice(i, i + limit)
    await Promise.all(batch.map(async (repo) => {
      const owner = repo.owner.login
      const name = repo.name

      const [issueList, prList, alertList] = await Promise.all([
        ghFetch(
          `/repos/${owner}/${name}/issues?state=open&per_page=100`,
          token,
          fetchImpl,
        ).catch(() => []),
        ghFetch(
          `/repos/${owner}/${name}/pulls?state=open&per_page=100`,
          token,
          fetchImpl,
        ).catch(() => []),
        ghFetch(
          `/repos/${owner}/${name}/dependabot/alerts?state=open&per_page=100`,
          token,
          fetchImpl,
        ).catch(() => []),
      ])

      const prNumbers = new Set((prList as any[]).map((p: any) => p.number))
      issues += (issueList as any[]).filter((i: any) => !prNumbers.has(i.number)).length
      pullRequests += prList.length
      dependabot += alertList.length
    }))
  }

  const notifications = (await notificationsPromise as any[]).length

  return {
    issues,
    pullRequests,
    prs: pullRequests,
    dependabot,
    securityAlerts: dependabot,
    notifications,
    authError: false,
  }
}

export function serviceFromGitHubStats(stats: GitHubStats): DashboardService {
  return {
    id: 'github',
    kind: 'code-host',
    provider: 'github',
    label: 'GitHub',
    status: stats.authError ? 'auth_error' : 'ok',
    counters: [
      { id: 'issues', label: 'Issues', value: stats.issues },
      { id: 'pullRequests', label: 'Pulls', value: stats.pullRequests },
      {
        id: 'securityAlerts',
        label: 'Security',
        value: stats.securityAlerts,
        ...(stats.securityAlerts > 0 ? { alert: true } : {}),
      },
      { id: 'notifications', label: 'Unread', value: stats.notifications },
    ],
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
      if (cache && Date.now() - cache.ts < CACHE_TTL_MS) {
        return serviceFromGitHubStats(cache.data)
      }

      try {
        const data = await fetchFresh(token, fetchImpl)
        cache = { data, ts: Date.now() }
        return serviceFromGitHubStats(data)
      } catch (err) {
        console.error('GitHub fetch error:', err)
        const data = cache
          ? { ...cache.data, authError: true }
          : {
              issues: 0,
              pullRequests: 0,
              prs: 0,
              dependabot: 0,
              securityAlerts: 0,
              notifications: 0,
              authError: true,
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
  }
}
