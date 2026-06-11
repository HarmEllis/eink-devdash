import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { test } from 'node:test'
import assert from 'node:assert/strict'

import { ClaudeCredentialStore } from './claude-credentials.js'

const OAUTH_TOKEN_ENDPOINT = 'https://platform.claude.com/v1/oauth/token'

async function withCredentials<T>(
  creds: Record<string, unknown>,
  fn: (path: string) => Promise<T>,
): Promise<T> {
  const home = await mkdtemp(join(tmpdir(), 'devdash-cred-test-'))
  const path = join(home, '.credentials.json')
  await writeFile(path, JSON.stringify({ claudeAiOauth: creds }), { mode: 0o600 })
  try {
    return await fn(path)
  } finally {
    await rm(home, { force: true, recursive: true })
  }
}

// Regression: a 401 on an unexpired access token must force a real OAuth
// refresh. Previously the refresh short-circuited on the still-valid clock and
// handed back the same rejected token (refreshHttpCalls stayed 0), so the GET
// and probe retries never recovered.
test('forceRefresh refreshes a server-rejected but unexpired token', async () => {
  const previousFetch = globalThis.fetch
  try {
    await withCredentials(
      {
        accessToken: 'rejected-but-unexpired',
        refreshToken: 'refresh-token',
        expiresAt: Date.now() + 60 * 60 * 1000,
        scopes: ['user:inference'],
      },
      async (path) => {
        let refreshCalls = 0
        globalThis.fetch = (async (input: unknown) => {
          if (String(input) === OAUTH_TOKEN_ENDPOINT) {
            refreshCalls += 1
            return new Response(
              JSON.stringify({ access_token: 'fresh-token', expires_in: 3600 }),
              { status: 200, headers: { 'content-type': 'application/json' } },
            )
          }
          return new Response(null, { status: 500 })
        }) as unknown as typeof fetch

        const store = new ClaudeCredentialStore(path)

        // Ordinary read returns the on-disk token without any network refresh.
        assert.equal(await store.getAccessToken(), 'rejected-but-unexpired')
        assert.equal(refreshCalls, 0)

        // forceRefresh (the 401 recovery path) must hit OAuth and yield a new token.
        store.invalidateCache()
        assert.equal(await store.getAccessToken({ forceRefresh: true }), 'fresh-token')
        assert.equal(refreshCalls, 1)

        const persisted = JSON.parse(await readFile(path, 'utf8'))
        assert.equal(persisted.claudeAiOauth.accessToken, 'fresh-token')
      },
    )
  } finally {
    globalThis.fetch = previousFetch
  }
})
