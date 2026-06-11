import { test } from 'node:test'
import assert from 'node:assert/strict'

import { serviceFromClaudeUsage, serviceFromCodexUsage } from './usage.adapters.js'

test('serviceFromClaudeUsage maps Claude limits to a generic usage service', () => {
  assert.deepEqual(
    serviceFromClaudeUsage({
      fiveHour: { used: 42, limit: 100, resetInSeconds: 300 },
      weekly: { used: 67, limit: 100, resetInSeconds: 600 },
      authError: false,
    }),
    {
      id: 'claude',
      kind: 'usage',
      provider: 'claude',
      label: 'Claude',
      status: 'ok',
      windows: [
        { id: 'fiveHour', label: '5h', used: 42, limit: 100, resetInSeconds: 300 },
        { id: 'weekly', label: '7d', used: 67, limit: 100, resetInSeconds: 600 },
      ],
    },
  )
})

const baseClaudeUsage = {
  fiveHour: { used: 42, limit: 100, resetInSeconds: 300 },
  weekly: { used: 67, limit: 100, resetInSeconds: 600 },
  authError: false,
}

test('serviceFromClaudeUsage emits a currency-aware extraUsage metric', () => {
  const service = serviceFromClaudeUsage({
    ...baseClaudeUsage,
    extraUsage: { amount: 0.91, percent: 5, limit: 17, currency: 'EUR' },
  })
  assert.deepEqual(service.metrics, [
    { id: 'extraUsage', label: '€', value: 0.91, valueText: '0,91', unit: 'EUR', usedPercent: 5, limit: 17 },
  ])
})

test('serviceFromClaudeUsage omits usedPercent/limit for the env-override shape', () => {
  const service = serviceFromClaudeUsage({
    ...baseClaudeUsage,
    extraUsage: { amount: 5, percent: null, limit: null, currency: 'USD' },
  })
  assert.deepEqual(service.metrics, [
    { id: 'extraUsage', label: '$', value: 5, valueText: '5', unit: 'USD' },
  ])
})

test('serviceFromClaudeUsage omits the extraUsage metric without overage spend', () => {
  for (const extraUsage of [
    null,
    undefined,
    { amount: 0, percent: 0, limit: 17, currency: 'EUR' },
  ]) {
    const service = serviceFromClaudeUsage({ ...baseClaudeUsage, extraUsage })
    assert.equal(service.metrics, undefined)
  }
})

test('serviceFromClaudeUsage omits unsupported/inherited currencies (never $)', () => {
  for (const currency of ['GBP', 'JPY', 'constructor', 'toString', '__proto__']) {
    const service = serviceFromClaudeUsage({
      ...baseClaudeUsage,
      extraUsage: { amount: 1.23, percent: 1, limit: 100, currency },
    })
    assert.equal(service.metrics, undefined, `currency ${currency} should be omitted`)
  }
})

test('serviceFromCodexUsage maps Codex limits to a generic usage service', () => {
  assert.deepEqual(
    serviceFromCodexUsage({
      status: 'ok',
      source: 'chatgpt',
      planType: 'plus',
      short: { usedPercent: 37, label: '5h', resetsAt: 1779232450, resetInSeconds: 300 },
      long: { usedPercent: 27, label: '7d', resetsAt: 1779641619, resetInSeconds: 600 },
      reachedLimit: 'short',
    }),
    {
      id: 'codex',
      kind: 'usage',
      provider: 'codex',
      label: 'Codex',
      status: 'ok',
      source: 'chatgpt',
      planType: 'plus',
      windows: [
        {
          id: 'short',
          label: '5h',
          usedPercent: 37,
          resetsAt: 1779232450,
          resetInSeconds: 300,
          reachedLimit: true,
        },
        {
          id: 'long',
          label: '7d',
          usedPercent: 27,
          resetsAt: 1779641619,
          resetInSeconds: 600,
          reachedLimit: false,
        },
      ],
    },
  )
})

const baseCodexUsage = {
  status: 'ok' as const,
  source: 'chatgpt',
  planType: 'plus',
  short: { usedPercent: 100, label: '5h', resetsAt: 1779232450, resetInSeconds: 300 },
  long: { usedPercent: 80, label: '7d', resetsAt: 1779641619, resetInSeconds: 600 },
  reachedLimit: 'short' as const,
}

test('serviceFromCodexUsage emits a USD extraUsage metric from the env amount', () => {
  // Codex has no live spend source, so the amount is env-driven with no
  // percent/limit (the device uses an amount-capped bar).
  const service = serviceFromCodexUsage({ ...baseCodexUsage, spend: 3 })
  assert.deepEqual(service.metrics, [
    { id: 'extraUsage', label: '$', value: 3, valueText: '3', unit: 'USD' },
  ])
})

test('serviceFromCodexUsage omits the extraUsage metric without overage spend', () => {
  for (const spend of [null, undefined, 0]) {
    const service = serviceFromCodexUsage({ ...baseCodexUsage, spend })
    assert.equal(service.metrics, undefined)
  }
})
