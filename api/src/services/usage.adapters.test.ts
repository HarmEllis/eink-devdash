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
