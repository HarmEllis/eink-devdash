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

  const adapter = createGitHubServiceAdapter({
    token: 'ghp_test',
    notificationsToken: 'ghp_notifications',
    fetch: fetchImpl,
  })
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

test('GitHub service adapter counts unread notifications across pages', async () => {
  const seenPaths: string[] = []
  const fetchImpl = async (url: string | URL | Request) => {
    const path = String(url).replace('https://api.github.com', '')
    seenPaths.push(path)

    if (path.startsWith('/user/repos')) {
      return Response.json([])
    }
    if (path === '/notifications?all=false&participating=false&per_page=100') {
      return Response.json(
        Array.from({ length: 100 }, (_, i) => ({ id: `n${i}` })),
        {
          headers: {
            link: '<https://api.github.com/notifications?all=false&participating=false&per_page=100&page=2>; rel="next"',
          },
        },
      )
    }
    if (path === '/notifications?all=false&participating=false&per_page=100&page=2') {
      return Response.json([{ id: 'n100' }, { id: 'n101' }])
    }
    return new Response(null, { status: 404 })
  }

  const adapter = createGitHubServiceAdapter({
    token: 'ghp_test',
    notificationsToken: 'ghp_notifications',
    fetch: fetchImpl,
  })
  const service = await adapter.getService()

  assert.equal(service?.status, 'ok')
  assert.equal(
    service?.counters?.find((counter) => counter.id === 'notifications')?.value,
    102,
  )
  assert.deepEqual(seenPaths.filter((path) => path.startsWith('/notifications')), [
    '/notifications?all=false&participating=false&per_page=100',
    '/notifications?all=false&participating=false&per_page=100&page=2',
  ])
})

test('GitHub service adapter omits notifications on notification scope errors', async () => {
  const fetchImpl = async (url: string | URL | Request) => {
    const path = String(url).replace('https://api.github.com', '')

    if (path.startsWith('/user/repos')) {
      return Response.json([])
    }
    if (path.startsWith('/notifications')) {
      return Response.json(
        { message: 'Resource not accessible by personal access token' },
        { status: 403 },
      )
    }
    return new Response(null, { status: 404 })
  }

  const adapter = createGitHubServiceAdapter({
    token: 'ghp_test',
    notificationsToken: 'ghp_limited',
    fetch: fetchImpl,
  })
  const service = await adapter.getService()

  assert.equal(service?.status, 'ok')
  assert.equal(
    service?.counters?.some((counter) => counter.id === 'notifications'),
    false,
  )
})

test('GitHub service adapter omits notifications when notification token is not configured', async () => {
  const fetchImpl = async (url: string | URL | Request) => {
    const path = String(url).replace('https://api.github.com', '')

    if (path.startsWith('/user/repos')) {
      return Response.json([])
    }
    if (path.startsWith('/notifications')) {
      throw new Error('notifications endpoint should not be called')
    }
    return new Response(null, { status: 404 })
  }

  const adapter = createGitHubServiceAdapter({ token: 'ghp_test', fetch: fetchImpl })
  const service = await adapter.getService()

  assert.equal(service?.status, 'ok')
  assert.equal(
    service?.counters?.some((counter) => counter.id === 'notifications'),
    false,
  )
})

test('GitHub service adapter skips repo permission errors without marking auth error', async () => {
  const fetchImpl = async (url: string | URL | Request) => {
    const path = String(url).replace('https://api.github.com', '')

    if (path.startsWith('/user/repos')) {
      return Response.json([
        { owner: { login: 'owner' }, name: 'ok-repo' },
        { owner: { login: 'owner' }, name: 'limited-repo' },
      ])
    }
    if (path.startsWith('/repos/owner/ok-repo/issues')) {
      return Response.json([{ number: 1 }])
    }
    if (path.startsWith('/repos/owner/ok-repo/pulls')) {
      return Response.json([])
    }
    if (path.startsWith('/repos/owner/ok-repo/dependabot/alerts')) {
      return Response.json([])
    }
    if (path.startsWith('/repos/owner/limited-repo/issues')) {
      return Response.json(
        { message: 'Resource not accessible by personal access token' },
        { status: 403 },
      )
    }
    if (path.startsWith('/repos/owner/limited-repo/pulls')) {
      return Response.json(
        { message: 'Resource not accessible by personal access token' },
        { status: 403 },
      )
    }
    if (path.startsWith('/repos/owner/limited-repo/dependabot/alerts')) {
      return Response.json(
        { message: 'Resource not accessible by personal access token' },
        { status: 403 },
      )
    }
    return new Response(null, { status: 404 })
  }

  const adapter = createGitHubServiceAdapter({ token: 'ghp_test', fetch: fetchImpl })
  const service = await adapter.getService()

  assert.equal(service?.status, 'ok')
  assert.equal(
    service?.counters?.find((counter) => counter.id === 'issues')?.value,
    1,
  )
  assert.equal(
    service?.counters?.find((counter) => counter.id === 'pullRequests')?.value,
    0,
  )
  assert.equal(
    service?.counters?.find((counter) => counter.id === 'securityAlerts')?.value,
    0,
  )
})

test('GitHub service adapter reports auth errors separately from upstream errors', async () => {
  const authAdapter = createGitHubServiceAdapter({
    token: 'bad',
    fetch: async () => Response.json({ message: 'Bad credentials' }, { status: 401 }),
  })
  const authService = await authAdapter.getService()
  assert.equal(authService?.status, 'auth_error')

  const errorAdapter = createGitHubServiceAdapter({
    token: 'ghp_test',
    fetch: async () => Response.json({ message: 'Server error' }, { status: 500 }),
  })
  const errorService = await errorAdapter.getService()
  assert.equal(errorService?.status, 'error')
})

test('GitHub service adapter is omitted when token is not configured', async () => {
  const adapter = createGitHubServiceAdapter({ token: '' })

  assert.equal(await adapter.getService(), null)
})
