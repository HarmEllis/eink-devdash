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
