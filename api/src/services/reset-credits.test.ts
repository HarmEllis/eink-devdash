import { test } from 'node:test'
import assert from 'node:assert/strict'
import { claudeResetCredits, codexResetCredits } from './reset-credits.js'

const NOW = Date.parse('2026-10-05T12:00:00Z')
const iso = (seconds: number) => new Date(NOW + seconds * 1000).toISOString()
const grant = (changes: Record<string, unknown> = {}) => ({
  resets_left: 2, resets_total: 3, paused: false,
  starts_at: iso(-3600), ends_at: iso(7200), ...changes,
})

test('Claude sums remaining resets and selects earliest active expiry, ignoring redemption gating', () => {
  assert.deepEqual(claudeResetCredits({ eligible: true, grants: [
    grant({ usable_now: false, use_requires_limit: true }),
    grant({ resets_left: 3, ends_at: iso(3600) }),
    grant({ paused: true }), grant({ starts_at: iso(60) }),
    grant({ ends_at: iso(0) }), grant({ resets_left: 0 }),
  ] }, NOW), { availableCount: 5, nextExpiresInSeconds: 3600 })
})

test('Claude distinguishes known zero from absent, ineligible and malformed inventories', () => {
  assert.deepEqual(claudeResetCredits({ eligible: true, grants: [] }, NOW),
    { availableCount: 0, nextExpiresInSeconds: null })
  assert.deepEqual(claudeResetCredits({ eligible: true, grants: [grant({ ends_at: iso(0) })] }, NOW),
    { availableCount: 0, nextExpiresInSeconds: null })
  for (const value of [undefined, null, {}, { eligible: false, grants: [] },
    { eligible: true, grants: null }, { eligible: true, grants: [grant({ resets_left: -1 })] },
    { eligible: true, grants: [grant({ resets_left: 1.5 })] },
    { eligible: true, grants: [grant({ resets_left: 4 })] },
    { eligible: true, grants: [grant({ ends_at: 'invalid' })] },
    { eligible: true, grants: [grant({ paused: 'false' })] },
    { eligible: true, grants: Array(201).fill(grant()) },
    { eligible: true, grants: [grant({ resets_left: 2147483647, resets_total: 2147483647 }), grant()] },
  ]) assert.equal(claudeResetCredits(value, NOW), undefined)
})

test('Codex keeps the authoritative count when details are incomplete and uses Unix-second expiries', () => {
  assert.deepEqual(codexResetCredits({ availableCount: 10, credits: [
    { status: 'available', expiresAt: NOW / 1000 + 7200 },
    { status: 'available', expiresAt: NOW / 1000 + 3600 },
    { status: 'redeemed', expiresAt: NOW / 1000 + 1 },
    { status: 'available', expiresAt: NOW / 1000 },
    { status: 'available', expiresAt: null },
    { status: 'available', expiresAt: '1234' },
  ] }, NOW), { availableCount: 10, nextExpiresInSeconds: 3600 })
})

test('Codex count-only, zero, unknown, malformed and old-binary responses remain distinct', () => {
  for (const credits of [undefined, null, []]) {
    assert.deepEqual(codexResetCredits({ availableCount: 2, credits }, NOW),
      { availableCount: 2, nextExpiresInSeconds: null })
    assert.deepEqual(codexResetCredits({ availableCount: 0, credits }, NOW),
      { availableCount: 0, nextExpiresInSeconds: null })
  }
  for (const value of [undefined, null, {}, { availableCount: -1 }, { availableCount: 0.5 },
    { availableCount: '2' }, { availableCount: 2147483648 }]) {
    assert.equal(codexResetCredits(value, NOW), undefined)
  }
})
