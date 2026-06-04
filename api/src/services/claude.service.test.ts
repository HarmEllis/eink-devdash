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

test('Claude 429 and transient probe failures are not reported as auth errors', async () => {
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

      globalThis.fetch = async () => new Response(null, { status: 503 })

      const usage = await getClaudeUsage()

      assert.equal(usage.authError, false)
      assert.deepEqual(usage.fiveHour, { used: 0, limit: 0, resetInSeconds: 0 })
      assert.deepEqual(usage.weekly, { used: 0, limit: 0, resetInSeconds: 0 })
    })
  } finally {
    globalThis.fetch = previousFetch
  }
})
