import { test } from 'node:test'
import assert from 'node:assert/strict'

import { codeHostConfigFromEnv, createCodeHostAdapters } from './code-host.adapters.js'
import { resolveCodeHostProvider } from './code-host.config.js'

test('resolveCodeHostProvider defaults to github only when GITHUB_TOKEN is configured', () => {
  assert.equal(resolveCodeHostProvider({ GITHUB_TOKEN: 'ghp_test' }), 'github')
  assert.equal(resolveCodeHostProvider({}), 'none')
})

test('resolveCodeHostProvider makes runtime code-host provider mutually exclusive', () => {
  assert.equal(
    resolveCodeHostProvider({ CODE_HOST_PROVIDER: 'none', GITHUB_TOKEN: 'ghp_test' }),
    'none',
  )
  assert.equal(
    resolveCodeHostProvider({ CODE_HOST_PROVIDER: 'gitlab', GITHUB_TOKEN: 'ghp_test' }),
    'gitlab',
  )
  assert.equal(
    resolveCodeHostProvider({ CODE_HOST_PROVIDER: 'github' }),
    'github',
  )
})

test('code-host adapter creation emits at most one runtime provider', () => {
  assert.deepEqual(codeHostConfigFromEnv({
    CODE_HOST_PROVIDER: 'github',
    GITHUB_TOKEN: 'ghp_repo',
    GITHUB_NOTIFICATIONS_TOKEN: 'ghp_notifications',
  }), {
    provider: 'github',
    token: 'ghp_repo',
    notificationsToken: 'ghp_notifications',
  })
  assert.deepEqual(codeHostConfigFromEnv({
    CODE_HOST_PROVIDER: 'gitlab',
    GITLAB_TOKEN: 'glpat_test',
    GITLAB_BASE_URL: 'https://gitlab.example.com',
  }), {
    provider: 'gitlab',
    token: 'glpat_test',
    baseUrl: 'https://gitlab.example.com',
  })
  assert.deepEqual(createCodeHostAdapters({ provider: 'gitlab' }), [])
  assert.deepEqual(createCodeHostAdapters({ provider: 'none' }), [])
  assert.deepEqual(
    createCodeHostAdapters({ provider: 'github', token: 'ghp_test' }).map((adapter) => adapter.id),
    ['github'],
  )
})
