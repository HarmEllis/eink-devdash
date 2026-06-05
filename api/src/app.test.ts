import { test } from 'node:test'
import assert from 'node:assert/strict'

import { createApp } from './app.js'
import type { DashboardServiceAdapter } from './services/dashboard-service.js'

const TOKEN = 'test-device-token'

// A stub adapter so /dashboard never reaches the real code-host/usage adapters
// (no network, no codex) — we only care about the auth + coexistence behavior.
const stubAdapters: DashboardServiceAdapter[] = [
  {
    id: 'stub',
    async getService() {
      return {
        id: 'stub',
        kind: 'code-host',
        provider: 'github',
        label: 'Stub',
        status: 'ok',
        counters: [{ id: 'x', label: 'X', value: 1 }],
      }
    },
  },
]

function buildApp() {
  // No RELAY_* env is set here: the direct route must work irrespective of any
  // relay configuration, which is exactly the coexistence guarantee.
  return createApp({ deviceToken: TOKEN, adapters: stubAdapters, logger: false })
}

test('GET /health needs no auth', async () => {
  const app = buildApp()
  const res = await app.inject({ method: 'GET', url: '/health' })
  assert.equal(res.statusCode, 200)
  assert.deepEqual(res.json(), { ok: true })
  await app.close()
})

test('GET /dashboard is rejected without a bearer token', async () => {
  const app = buildApp()
  const res = await app.inject({ method: 'GET', url: '/dashboard' })
  assert.equal(res.statusCode, 401)
  await app.close()
})

test('GET /dashboard is rejected with a wrong token', async () => {
  const app = buildApp()
  const res = await app.inject({
    method: 'GET',
    url: '/dashboard',
    headers: { authorization: 'Bearer wrong' },
  })
  assert.equal(res.statusCode, 401)
  await app.close()
})

test('GET /dashboard succeeds with the device token while no relay is configured', async () => {
  const app = buildApp()
  const res = await app.inject({
    method: 'GET',
    url: '/dashboard',
    headers: { authorization: `Bearer ${TOKEN}` },
  })
  assert.equal(res.statusCode, 200)
  const body = res.json() as { schemaVersion: number; services: unknown[] }
  assert.equal(body.schemaVersion, 2)
  assert.equal(body.services.length, 1)
  await app.close()
})
