import { test } from 'node:test'
import assert from 'node:assert/strict'

import { createDashboardCoordinator } from './dashboard-coordinator.js'
import type { DashboardPayload } from './routes/dashboard.js'

const PAYLOAD: DashboardPayload = {
  schemaVersion: 2,
  services: [],
  updatedAt: '2026-06-07T10:00:00.000Z',
  updatedAtLocal: '12:00',
  updatedAtLocalIso: '2026-06-07T12:00:00',
}

test('coalesces callers onto one underlying build', async () => {
  let builds = 0
  let resolve!: (payload: DashboardPayload) => void
  const coordinator = createDashboardCoordinator([], 'UTC', {
    build: async () => {
      builds += 1
      return new Promise<DashboardPayload>((done) => { resolve = done })
    },
  })
  const first = coordinator.getDashboard()
  const second = coordinator.getDashboard()
  assert.equal(builds, 1)
  resolve(PAYLOAD)
  assert.deepEqual(await first, PAYLOAD)
  assert.deepEqual(await second, PAYLOAD)
})

test('a caller abort rejects only that caller', async () => {
  let resolve!: (payload: DashboardPayload) => void
  let buildSignal: AbortSignal | undefined
  const coordinator = createDashboardCoordinator([], 'UTC', {
    build: async (_now, _adapters, _tz, opts) => {
      buildSignal = opts.signal
      return new Promise<DashboardPayload>((done) => { resolve = done })
    },
  })
  const caller = new AbortController()
  const aborted = coordinator.getDashboard(caller.signal)
  const peer = coordinator.getDashboard()
  caller.abort(new Error('caller stopped'))
  await assert.rejects(aborted, /caller stopped/)
  assert.equal(buildSignal?.aborted, false)
  resolve(PAYLOAD)
  assert.deepEqual(await peer, PAYLOAD)
})

test('budget aborts work, keeps admission closed, and invokes backstop', async () => {
  let builds = 0
  let unrecoverable = 0
  let observedSignal: AbortSignal | undefined
  const coordinator = createDashboardCoordinator([], 'UTC', {
    budgetMs: 5,
    abortGraceMs: 5,
    onUnrecoverable: () => { unrecoverable += 1 },
    build: async (_now, _adapters, _tz, opts) => {
      builds += 1
      observedSignal = opts.signal
      return new Promise<DashboardPayload>(() => {})
    },
  })
  await assert.rejects(coordinator.getDashboard(), /budget/)
  await assert.rejects(coordinator.getDashboard(), /budget/)
  assert.equal(builds, 1)
  assert.equal(observedSignal?.aborted, true)
  await new Promise((resolve) => setTimeout(resolve, 10))
  assert.equal(unrecoverable, 1)
})
