const GITHUB_TOKEN = process.env.GITHUB_TOKEN
const BASE = 'https://api.github.com'
const CACHE_TTL_MS = 60_000

type GitHubStats = {
  issues: number
  prs: number
  dependabot: number
  authError: boolean
}
let cache: { data: GitHubStats; ts: number } | null = null

async function ghFetch(path: string) {
  const res = await fetch(`${BASE}${path}`, {
    headers: {
      Authorization: `Bearer ${GITHUB_TOKEN}`,
      Accept: 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
    },
  })
  if (!res.ok) throw new Error(`GitHub ${res.status}: ${path}`)
  return res.json() as Promise<any>
}

async function fetchFresh(): Promise<GitHubStats> {
  const repos: any[] = await ghFetch('/user/repos?per_page=100&type=owner')

  const limit = 5
  let issues = 0, prs = 0, dependabot = 0

  for (let i = 0; i < repos.length; i += limit) {
    const batch = repos.slice(i, i + limit)
    await Promise.all(batch.map(async (repo) => {
      const owner = repo.owner.login
      const name = repo.name

      const [issueList, prList, alertList] = await Promise.all([
        ghFetch(`/repos/${owner}/${name}/issues?state=open&per_page=100`).catch(() => []),
        ghFetch(`/repos/${owner}/${name}/pulls?state=open&per_page=100`).catch(() => []),
        ghFetch(`/repos/${owner}/${name}/dependabot/alerts?state=open&per_page=100`).catch(() => []),
      ])

      const prNumbers = new Set((prList as any[]).map((p: any) => p.number))
      issues += (issueList as any[]).filter((i: any) => !prNumbers.has(i.number)).length
      prs += prList.length
      dependabot += alertList.length
    }))
  }

  return { issues, prs, dependabot, authError: false }
}

export async function getGitHubStats(): Promise<GitHubStats> {
  if (!GITHUB_TOKEN) return { issues: 0, prs: 0, dependabot: 0, authError: true }
  if (cache && Date.now() - cache.ts < CACHE_TTL_MS) return cache.data

  try {
    const data = await fetchFresh()
    cache = { data, ts: Date.now() }
    return data
  } catch (err) {
    console.error('GitHub fetch error:', err)
    if (cache) return { ...cache.data, authError: true }
    return { issues: 0, prs: 0, dependabot: 0, authError: true }
  }
}
