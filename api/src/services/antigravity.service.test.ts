import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  getAntigravityUsage,
  parseAntigravityQuotaSummary,
} from './antigravity.service.js'

const NOW = Date.parse('2026-06-13T20:00:00Z')

function quotaResponse(): Record<string, unknown> {
  return {
    groups: [
      {
        displayName: 'Gemini',
        buckets: [
          {
            window: 'weekly',
            remainingFraction: 0.9238627,
            resetTime: '2026-06-20T20:00:00Z',
          },
          {
            window: '5h',
            remainingFraction: 0.5431762,
            resetTime: '2026-06-13T23:00:00Z',
          },
        ],
      },
      {
        displayName: 'Claude/GPT',
        buckets: [
          {
            window: 'weekly',
            remainingFraction: 0.6579987,
            resetTime: '2026-06-19T20:00:00Z',
          },
          {
            window: '5h',
            remainingFraction: 0,
            resetTime: '2026-06-13T22:00:00Z',
          },
        ],
      },
    ],
  }
}

async function withAntigravityHome<T>(fn: () => Promise<T>): Promise<T> {
  const home = await mkdtemp(join(tmpdir(), 'devdash-antigravity-test-'))
  const previous = {
    home: process.env.HOME,
    token: process.env.ANTIGRAVITY_TOKEN,
    tokenPath: process.env.ANTIGRAVITY_TOKEN_PATH,
  }

  process.env.HOME = home
  delete process.env.ANTIGRAVITY_TOKEN
  delete process.env.ANTIGRAVITY_TOKEN_PATH

  try {
    const authDir = join(home, '.gemini', 'antigravity-cli')
    await mkdir(authDir, { recursive: true })
    await writeFile(
      join(authDir, 'antigravity-oauth-token'),
      JSON.stringify({
        token: {
          access_token: 'expired-access-token',
          refresh_token: 'test-refresh-token',
          expiry: '2026-01-01T00:00:00Z',
        },
      }),
    )
    return await fn()
  } finally {
    if (previous.home === undefined) delete process.env.HOME
    else process.env.HOME = previous.home
    if (previous.token === undefined) delete process.env.ANTIGRAVITY_TOKEN
    else process.env.ANTIGRAVITY_TOKEN = previous.token
    if (previous.tokenPath === undefined) delete process.env.ANTIGRAVITY_TOKEN_PATH
    else process.env.ANTIGRAVITY_TOKEN_PATH = previous.tokenPath
    await rm(home, { force: true, recursive: true })
  }
}

test('parseAntigravityQuotaSummary selects the binding model group per window', () => {
  assert.deepEqual(parseAntigravityQuotaSummary(quotaResponse(), NOW), {
    short: {
      label: '5h',
      usedPercent: 100,
      resetsAt: Date.parse('2026-06-13T22:00:00Z') / 1000,
      resetInSeconds: 2 * 60 * 60,
    },
    long: {
      label: '7d',
      usedPercent: 34.2,
      resetsAt: Date.parse('2026-06-19T20:00:00Z') / 1000,
      resetInSeconds: 6 * 24 * 60 * 60,
    },
    reachedLimit: 'short',
    groups: [
      {
        id: 'gemini',
        label: 'Gemini',
        short: {
          label: '5h',
          usedPercent: 45.68,
          resetsAt: Date.parse('2026-06-13T23:00:00Z') / 1000,
          resetInSeconds: 3 * 60 * 60,
        },
        long: {
          label: '7d',
          usedPercent: 7.61,
          resetsAt: Date.parse('2026-06-20T20:00:00Z') / 1000,
          resetInSeconds: 7 * 24 * 60 * 60,
        },
        reachedLimit: null,
      },
      {
        id: 'claudeGpt',
        label: 'Claude/GPT',
        short: {
          label: '5h',
          usedPercent: 100,
          resetsAt: Date.parse('2026-06-13T22:00:00Z') / 1000,
          resetInSeconds: 2 * 60 * 60,
        },
        long: {
          label: '7d',
          usedPercent: 34.2,
          resetsAt: Date.parse('2026-06-19T20:00:00Z') / 1000,
          resetInSeconds: 6 * 24 * 60 * 60,
        },
        reachedLimit: 'short',
      },
    ],
  })
})

test('parseAntigravityQuotaSummary accepts the local language-server wrapper', () => {
  assert.equal(
    parseAntigravityQuotaSummary({ response: quotaResponse() }, NOW).long.usedPercent,
    34.2,
  )
})

test('getAntigravityUsage refreshes an expired token and retrieves live quotas', async () => {
  const previousFetch = globalThis.fetch

  try {
    await withAntigravityHome(async () => {
      const requests: string[] = []
      globalThis.fetch = (async (input: string | URL | Request, init?: RequestInit) => {
        const url = String(input)
        requests.push(url)

        if (url.includes('oauth2.googleapis.com/token')) {
          assert.match(String(init?.body), /refresh_token=test-refresh-token/)
          return Response.json({ access_token: 'fresh-access-token' })
        }

        assert.equal(
          new Headers(init?.headers).get('authorization'),
          'Bearer fresh-access-token',
        )
        return Response.json(quotaResponse())
      }) as typeof fetch

      const usage = await getAntigravityUsage()

      assert.equal(usage.status, 'ok')
      assert.equal(usage.short.usedPercent, 100)
      assert.equal(usage.long.usedPercent, 34.2)
      assert.equal(usage.reachedLimit, 'short')
      assert.equal(usage.groups.length, 2)
      assert.equal(requests.length, 2)
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})

test('getAntigravityUsage reports rejected credentials as an auth error', async () => {
  const previousFetch = globalThis.fetch
  const previousToken = process.env.ANTIGRAVITY_TOKEN

  try {
    process.env.ANTIGRAVITY_TOKEN = 'invalid-access-token'
    globalThis.fetch = (async () => new Response(null, { status: 401 })) as typeof fetch

    const usage = await getAntigravityUsage()

    assert.equal(usage.status, 'auth_error')
    assert.equal(usage.short.usedPercent, 0)
    assert.equal(usage.long.usedPercent, 0)
    assert.deepEqual(usage.groups, [])
  } finally {
    if (previousToken === undefined) delete process.env.ANTIGRAVITY_TOKEN
    else process.env.ANTIGRAVITY_TOKEN = previousToken
    globalThis.fetch = previousFetch
  }
})

function argonGroup(remainingFraction: number): Record<string, unknown> {
  return {
    displayName: 'Gemini 4 Argon',
    buckets: [
      {
        bucketId: 'gemini-argon-wtu-weekly',
        window: 'weekly',
        remainingFraction,
        resetTime: '2026-06-15T08:00:00Z',
      },
    ],
  }
}

for (const position of ['before', 'after'] as const) {
  test(`parseAntigravityQuotaSummary keeps a weekly-only Argon group ${position} Gemini Models out of the Gemini tile`, () => {
    const payload = quotaResponse()
    const groups = payload.groups as Record<string, unknown>[]
    groups[0].displayName = 'Gemini Models'
    groups[1].displayName = 'Claude and GPT models'
    if (position === 'before') groups.unshift(argonGroup(0))
    else groups.push(argonGroup(0))

    const parsed = parseAntigravityQuotaSummary(payload, NOW)

    assert.deepEqual(
      parsed.groups.map((group) => group.id),
      ['gemini', 'claudeGpt'],
    )
    const gemini = parsed.groups[0]
    assert.equal(gemini.label, 'Gemini')
    assert.equal(gemini.short.usedPercent, 45.68)
    assert.equal(gemini.long.usedPercent, 7.61)
    assert.equal(gemini.long.resetsAt, Date.parse('2026-06-20T20:00:00Z') / 1000)
    assert.equal(gemini.reachedLimit, null)
  })
}

test('parseAntigravityQuotaSummary does not create a tile from a weekly-only group', () => {
  const payload = quotaResponse()
  const groups = payload.groups as Record<string, unknown>[]
  groups.splice(0, 1, argonGroup(0.5))

  const parsed = parseAntigravityQuotaSummary(payload, NOW)

  assert.deepEqual(
    parsed.groups.map((group) => group.id),
    ['claudeGpt'],
  )
})

async function requestedUserAgent(override: string | undefined): Promise<string | null> {
  const previousFetch = globalThis.fetch
  const previousToken = process.env.ANTIGRAVITY_TOKEN
  const previousUserAgent = process.env.ANTIGRAVITY_USER_AGENT
  let userAgent: string | null = null

  try {
    process.env.ANTIGRAVITY_TOKEN = 'test-access-token'
    if (override === undefined) delete process.env.ANTIGRAVITY_USER_AGENT
    else process.env.ANTIGRAVITY_USER_AGENT = override
    globalThis.fetch = (async (_input: string | URL | Request, init?: RequestInit) => {
      userAgent = new Headers(init?.headers).get('user-agent')
      return Response.json(quotaResponse())
    }) as typeof fetch

    assert.equal((await getAntigravityUsage()).status, 'ok')
    return userAgent
  } finally {
    if (previousToken === undefined) delete process.env.ANTIGRAVITY_TOKEN
    else process.env.ANTIGRAVITY_TOKEN = previousToken
    if (previousUserAgent === undefined) delete process.env.ANTIGRAVITY_USER_AGENT
    else process.env.ANTIGRAVITY_USER_AGENT = previousUserAgent
    globalThis.fetch = previousFetch
  }
}

test('getAntigravityUsage presents as the current Antigravity CLI by default', async () => {
  assert.equal(
    await requestedUserAgent(undefined),
    `antigravity/cli/1.4.0 ${process.platform}/${process.arch}`,
  )
  assert.equal(
    await requestedUserAgent('   '),
    `antigravity/cli/1.4.0 ${process.platform}/${process.arch}`,
  )
})

test('getAntigravityUsage uses ANTIGRAVITY_USER_AGENT when set', async () => {
  assert.equal(
    await requestedUserAgent(' antigravity/cli/9.9.9 linux/x64 '),
    'antigravity/cli/9.9.9 linux/x64',
  )
})
