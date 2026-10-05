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

test('Claude requests reset grants without dropping spend and preserves them through the window probe', async () => {
  const previousFetch = globalThis.fetch
  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')
      let queryCalls = 0
      globalThis.fetch = (async (input: unknown, init?: RequestInit) => {
        if (String(input).includes('/api/oauth/usage')) {
          assert.ok(String(input).endsWith('?cedar_ember=1'))
          assert.equal(new Headers(init?.headers).get('User-Agent'), 'claude-cli/2.1.280 (external, cli)')
          queryCalls++
          return usageBody({
            // Deliberately no quota windows: the existing probe supplies them.
            extra_usage: { is_enabled: true, used_credits: 91, monthly_limit: 1700, currency: 'EUR' },
            cedar_ember: { eligible: true, grants: [{ resets_left: 2, resets_total: 3, paused: false,
              starts_at: new Date(Date.now() - 3600000).toISOString(),
              ends_at: new Date(Date.now() + 39600000).toISOString(), usable_now: false }] },
          })
        }
        return new Response(null, { status: 200, headers: {
          'anthropic-ratelimit-unified-5h-utilization': '0.5',
          'anthropic-ratelimit-unified-5h-reset': String(Math.floor(Date.now() / 1000) + 600),
        } })
      }) as typeof fetch
      const usage = await getClaudeUsage()
      assert.equal(queryCalls, 1)
      assert.equal(usage.fiveHour.used, 50)
      assert.equal(usage.authError, false)
      assert.equal(usage.extraUsage!.amount, 0.91)
      assert.equal(usage.resetCredits!.availableCount, 2)
      assert.ok(usage.resetCredits!.nextExpiresInSeconds! >= 39599)
    })
  } finally { globalThis.fetch = previousFetch }
})

test('Claude optional query rejection retries ordinary GET and remembers the cooldown', async () => {
  const previousFetch = globalThis.fetch
  const { resetClaudeUsageQueryCooldownForTests } = await import('./claude.service.js')
  resetClaudeUsageQueryCooldownForTests()
  try {
    await withClaudeHome(async () => {
      const { getClaudeUsage } = await import('./claude.service.js')
      const requests: string[] = []
      globalThis.fetch = (async (input: unknown) => {
        const url = String(input)
        requests.push(url)
        if (url.includes('?')) return new Response(null, { status: 403 })
        return usageBody({ five_hour: { utilization: 17 }, extra_usage: {
          is_enabled: true, used_credits: 91, monthly_limit: 1700, currency: 'EUR',
        } })
      }) as typeof fetch
      for (let i = 0; i < 2; i++) {
        const usage = await getClaudeUsage()
        assert.equal(usage.authError, false)
        assert.equal(usage.fiveHour.used, 17)
        assert.equal(usage.extraUsage!.amount, 0.91)
        assert.equal(usage.resetCredits, undefined)
      }
      assert.deepEqual(requests, [
        'https://api.anthropic.com/api/oauth/usage?cedar_ember=1',
        'https://api.anthropic.com/api/oauth/usage',
        'https://api.anthropic.com/api/oauth/usage',
      ])
    })
  } finally {
    globalThis.fetch = previousFetch
    resetClaudeUsageQueryCooldownForTests()
  }
})

test('Claude unsupported optional-query statuses fall back, while 429 never retries the GET', async () => {
  const previousFetch = globalThis.fetch
  const { getClaudeUsage, resetClaudeUsageQueryCooldownForTests } = await import('./claude.service.js')
  try {
    await withClaudeHome(async () => {
      for (const status of [400, 404, 422, 429]) {
        resetClaudeUsageQueryCooldownForTests()
        let reads = 0
        globalThis.fetch = (async (input: unknown) => {
          if (String(input).includes('/api/oauth/usage')) {
            reads++
            return reads === 1 ? new Response(null, { status }) : usageBody({ five_hour: { utilization: 17 } })
          }
          return new Response(null, { status: 429, headers: { 'retry-after': '300' } })
        }) as typeof fetch
        const usage = await getClaudeUsage()
        assert.equal(reads, status === 429 ? 1 : 2)
        assert.equal(usage.authError, false)
        assert.equal(usage.fiveHour.used, status === 429 ? 100 : 17)
      }
    })
  } finally {
    globalThis.fetch = previousFetch
    resetClaudeUsageQueryCooldownForTests()
  }
})
