import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { test } from 'node:test'
import assert from 'node:assert/strict'

async function withClaudeHome<T>(fn: () => Promise<T>): Promise<T> {
  const home = await mkdtemp(join(tmpdir(), 'devdash-claude-test-'))
  const previousHome = process.env.HOME
  process.env.HOME = home

  try {
    const claudeDir = join(home, '.claude')
    await mkdir(claudeDir)
    await writeFile(
      join(claudeDir, '.credentials.json'),
      JSON.stringify({
        claudeAiOauth: {
          accessToken: 'test-access-token',
          refreshToken: 'test-refresh-token',
          expiresAt: Date.now() + 60 * 60 * 1000,
        },
      }),
    )

    return await fn()
  } finally {
    if (previousHome === undefined) delete process.env.HOME
    else process.env.HOME = previousHome
    await rm(home, { force: true, recursive: true })
  }
}

test('Claude 429 probe is reported as usage, not auth error', async () => {
  const previousFetch = globalThis.fetch

  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')

      globalThis.fetch = async () => new Response(null, {
        status: 429,
        headers: { 'retry-after': '300' },
      })

      const fallbackUsage = await getClaudeUsage()

      assert.equal(fallbackUsage.authError, false)
      assert.deepEqual(fallbackUsage.fiveHour, { used: 100, limit: 100, resetInSeconds: 300 })
      assert.deepEqual(fallbackUsage.weekly, { used: 100, limit: 100, resetInSeconds: 300 })

      const reset = Math.floor(Date.now() / 1000) + 600

      globalThis.fetch = async () => new Response(null, {
        status: 429,
        headers: {
          'anthropic-ratelimit-unified-5h-utilization': '1',
          'anthropic-ratelimit-unified-5h-reset': String(reset),
          'anthropic-ratelimit-unified-7d-utilization': '0.45',
          'anthropic-ratelimit-unified-7d-reset': String(reset + 3600),
        },
      })

      const headerUsage = await getClaudeUsage()

      assert.equal(headerUsage.authError, false)
      assert.equal(headerUsage.fiveHour.used, 100)
      assert.equal(headerUsage.fiveHour.limit, 100)
      assert.equal(headerUsage.weekly.used, 45)
      assert.equal(headerUsage.weekly.limit, 100)
      assert.ok(headerUsage.fiveHour.resetInSeconds > 0)
      assert.ok(headerUsage.weekly.resetInSeconds > headerUsage.fiveHour.resetInSeconds)
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})

type FetchRoutes = {
  usage?: () => Response | Promise<Response>
  probe?: () => Response | Promise<Response>
}

// Routes the free GET /api/oauth/usage separately from the billed POST probe,
// and records whether the probe was hit so tests can assert it stays unused.
function routedFetch(routes: FetchRoutes): { fetch: typeof fetch; probeCalled: () => boolean } {
  let probeCalled = false
  const fetch = (async (input: unknown) => {
    const url = String(input)
    if (url.includes('/api/oauth/usage')) {
      return routes.usage ? routes.usage() : new Response(null, { status: 500 })
    }
    probeCalled = true
    return routes.probe ? routes.probe() : new Response(null, { status: 500 })
  }) as unknown as typeof fetch
  return { fetch, probeCalled: () => probeCalled }
}

function usageBody(body: Record<string, unknown>): Response {
  return new Response(JSON.stringify(body), {
    status: 200,
    headers: { 'content-type': 'application/json' },
  })
}

test('Claude reads windows + extra usage from the free GET and skips the billed probe', async () => {
  const previousFetch = globalThis.fetch
  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')
      const routed = routedFetch({
        usage: () =>
          usageBody({
            five_hour: { utilization: 33, resets_at: new Date(Date.now() + 300_000).toISOString() },
            seven_day: { utilization: 21, resets_at: new Date(Date.now() + 600_000).toISOString() },
            extra_usage: {
              is_enabled: true,
              monthly_limit: 1700,
              used_credits: 91,
              utilization: 5.35,
              currency: 'EUR',
            },
          }),
      })
      globalThis.fetch = routed.fetch

      const usage = await getClaudeUsage()

      assert.equal(usage.authError, false)
      assert.equal(usage.fiveHour.used, 33)
      assert.equal(usage.weekly.used, 21)
      assert.ok(usage.fiveHour.resetInSeconds > 0)
      assert.deepEqual(usage.extraUsage, { amount: 0.91, percent: 5, limit: 17, currency: 'EUR' })
      assert.equal(routed.probeCalled(), false)
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})

test('Claude omits extra usage when disabled or in an unsupported currency', async () => {
  const previousFetch = globalThis.fetch
  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')
      const windows = {
        five_hour: { utilization: 10, resets_at: new Date(Date.now() + 300_000).toISOString() },
        seven_day: { utilization: 5, resets_at: new Date(Date.now() + 600_000).toISOString() },
      }

      const disabled = routedFetch({
        usage: () =>
          usageBody({
            ...windows,
            extra_usage: { is_enabled: false, monthly_limit: 1700, used_credits: 91, currency: 'EUR' },
          }),
      })
      globalThis.fetch = disabled.fetch
      assert.equal((await getClaudeUsage()).extraUsage, null)
      assert.equal(disabled.probeCalled(), false)

      const unsupported = routedFetch({
        usage: () =>
          usageBody({
            ...windows,
            extra_usage: {
              is_enabled: true,
              monthly_limit: 1700,
              used_credits: 91,
              utilization: 5,
              currency: 'GBP',
            },
          }),
      })
      globalThis.fetch = unsupported.fetch
      assert.equal((await getClaudeUsage()).extraUsage, null)
      assert.equal(unsupported.probeCalled(), false)
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})

test('Claude falls back to the probe for windows but preserves the GET spend', async () => {
  const previousFetch = globalThis.fetch
  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')
      const reset = Math.floor(Date.now() / 1000) + 600
      const routed = routedFetch({
        // GET has valid spend but no usable windows.
        usage: () =>
          usageBody({
            extra_usage: {
              is_enabled: true,
              monthly_limit: 1700,
              used_credits: 91,
              utilization: 5,
              currency: 'EUR',
            },
          }),
        probe: () =>
          new Response(null, {
            status: 200,
            headers: {
              'anthropic-ratelimit-unified-5h-utilization': '0.6',
              'anthropic-ratelimit-unified-5h-reset': String(reset),
              'anthropic-ratelimit-unified-7d-utilization': '0.2',
              'anthropic-ratelimit-unified-7d-reset': String(reset),
            },
          }),
      })
      globalThis.fetch = routed.fetch

      const usage = await getClaudeUsage()

      assert.equal(routed.probeCalled(), true)
      assert.equal(usage.fiveHour.used, 60)
      assert.equal(usage.weekly.used, 20)
      assert.deepEqual(usage.extraUsage, { amount: 0.91, percent: 5, limit: 17, currency: 'EUR' })
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})

test('Claude falls back to the probe when the GET is unavailable (no spend source)', async () => {
  const previousFetch = globalThis.fetch
  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')
      const reset = Math.floor(Date.now() / 1000) + 300
      const routed = routedFetch({
        usage: () => new Response(null, { status: 500 }),
        probe: () =>
          new Response(null, {
            status: 200,
            headers: {
              'anthropic-ratelimit-unified-5h-utilization': '0.5',
              'anthropic-ratelimit-unified-5h-reset': String(reset),
              'anthropic-ratelimit-unified-7d-utilization': '0.1',
              'anthropic-ratelimit-unified-7d-reset': String(reset),
            },
          }),
      })
      globalThis.fetch = routed.fetch

      const usage = await getClaudeUsage()

      assert.equal(routed.probeCalled(), true)
      assert.equal(usage.fiveHour.used, 50)
      assert.equal(usage.extraUsage ?? null, null)
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})
