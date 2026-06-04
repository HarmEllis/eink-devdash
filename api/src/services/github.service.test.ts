import { test } from 'node:test'
import assert from 'node:assert/strict'

import { createGitHubServiceAdapter } from './github.service.js'

test('GitHub service adapter returns generic code-host counters including notifications', async () => {
  const seenPaths: string[] = []
  const fetchImpl = async (url: string | URL | Request) => {
    const path = String(url).replace('https://api.github.com', '')
    seenPaths.push(path)

    if (path.startsWith('/user/repos')) {
      return Response.json([{ owner: { login: 'owner' }, name: 'repo' }])
    }
    if (path.startsWith('/repos/owner/repo/issues')) {
      return Response.json([{ number: 1 }, { number: 2 }])
    }
    if (path.startsWith('/repos/owner/repo/pulls')) {
      return Response.json([{ number: 2 }])
    }
    if (path.startsWith('/repos/owner/repo/dependabot/alerts')) {
      return Response.json([{ id: 1 }, { id: 2 }])
    }
    if (path.startsWith('/notifications')) {
      return Response.json([{ id: 'n1' }, { id: 'n2' }, { id: 'n3' }])
    }
    return new Response(null, { status: 404 })
  }

  const adapter = createGitHubServiceAdapter({ token: 'ghp_test', fetch: fetchImpl })
  const service = await adapter.getService()

  assert.deepEqual(service, {
    id: 'github',
    kind: 'code-host',
    provider: 'github',
    label: 'GitHub',
    status: 'ok',
    counters: [
      { id: 'issues', label: 'Issues', value: 1 },
      { id: 'pullRequests', label: 'Pulls', value: 1 },
      { id: 'securityAlerts', label: 'Security', value: 2, alert: true },
      { id: 'notifications', label: 'Unread', value: 3 },
    ],
  })
  assert.ok(seenPaths.some((path) => path.startsWith('/notifications')))
})

test('GitHub service adapter is omitted when token is not configured', async () => {
  const adapter = createGitHubServiceAdapter({ token: '' })

  assert.equal(await adapter.getService(), null)
})
