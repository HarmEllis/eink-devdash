import { test, expect } from 'vitest'

import {
  base64url,
  buildRelayDashboardUrl,
  formatProvisioningSummary,
  generateIdentity,
  generateToken,
  mergeEnv,
  parseWorkerUrl,
  relayEnvUpdates,
} from '../scripts/setup-helpers.mjs'

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i

test('base64url emits URL-safe output without padding', () => {
  // 0xFB 0xFF 0xFE would be "+//+" in std base64 — assert the url-safe mapping.
  const encoded = base64url(new Uint8Array([0xfb, 0xff, 0xbf]))
  expect(encoded).not.toMatch(/[+/=]/)
  expect(encoded).toBe('-_-_')
})

test('generateToken returns a high-entropy url-safe string', () => {
  const a = generateToken()
  const b = generateToken()
  expect(a).not.toBe(b)
  expect(a).not.toMatch(/[+/=]/)
  // 32 bytes -> 43 base64url chars.
  expect(a.length).toBeGreaterThanOrEqual(42)
})

test('generateIdentity yields a v4 uuid and three distinct secrets', () => {
  const id = generateIdentity()
  expect(id.deviceUuid).toMatch(UUID_RE)
  const secrets = [id.deviceToken, id.relayPublishKey, id.adminKey]
  expect(new Set(secrets).size).toBe(3)
})

test('mergeEnv updates existing keys in place and preserves comments', () => {
  const existing = [
    '# Required',
    'DEVICE_TOKEN=old-token',
    '',
    '# Optional',
    'MDNS_NAME=devdash-api',
    '',
  ].join('\n')

  const merged = mergeEnv(existing, { DEVICE_TOKEN: 'new-token', RELAY_ENABLED: 'true' })

  expect(merged).toContain('# Required')
  expect(merged).toContain('# Optional')
  expect(merged).toContain('DEVICE_TOKEN=new-token')
  expect(merged).not.toContain('old-token')
  // Untouched key stays.
  expect(merged).toContain('MDNS_NAME=devdash-api')
  // New key appended, single trailing newline, no blank-line gap before it.
  expect(merged.endsWith('RELAY_ENABLED=true\n')).toBe(true)
})

test('mergeEnv only matches whole keys, not substrings or commented lines', () => {
  const existing = ['#DEVICE_TOKEN=commented', 'MY_DEVICE_TOKEN=keep'].join('\n')
  const merged = mergeEnv(existing, { DEVICE_TOKEN: 'fresh' })
  expect(merged).toContain('#DEVICE_TOKEN=commented')
  expect(merged).toContain('MY_DEVICE_TOKEN=keep')
  expect(merged).toContain('\nDEVICE_TOKEN=fresh\n')
})

test('mergeEnv creates content from empty input', () => {
  expect(mergeEnv('', { DEVICE_TOKEN: 'x' })).toBe('DEVICE_TOKEN=x\n')
})

test('relayEnvUpdates includes DEVICE_TOKEN so the API can start', () => {
  const updates = relayEnvUpdates({
    deviceUuid: 'uuid',
    deviceToken: 'tok',
    relayPublishKey: 'pub',
    workerUrl: 'https://relay.example.workers.dev',
  })
  expect(updates).toEqual({
    DEVICE_TOKEN: 'tok',
    DEVICE_UUID: 'uuid',
    RELAY_ENABLED: 'true',
    RELAY_URL: 'https://relay.example.workers.dev',
    RELAY_PUBLISH_KEY: 'pub',
  })
})

test('buildRelayDashboardUrl points at /d/<uuid> and drops query/hash', () => {
  expect(
    buildRelayDashboardUrl('https://relay.example.workers.dev/anything?x=1#y', 'abc-123'),
  ).toBe('https://relay.example.workers.dev/d/abc-123')
})

test('parseWorkerUrl extracts the workers.dev URL from deploy output', () => {
  const stdout = [
    'Total Upload: 12.34 KiB / gzip: 4.56 KiB',
    'Uploaded eink-devdash-relay (1.23 sec)',
    'Published eink-devdash-relay (0.98 sec)',
    '  https://eink-devdash-relay.joost.workers.dev',
    'Current Deployment ID: abc',
  ].join('\n')
  expect(parseWorkerUrl(stdout)).toBe('https://eink-devdash-relay.joost.workers.dev')
})

test('parseWorkerUrl returns null when no workers.dev URL is present', () => {
  expect(parseWorkerUrl('Deployed to custom domain relay.example.com')).toBeNull()
  expect(parseWorkerUrl('')).toBeNull()
})

test('formatProvisioningSummary shows the URL, token, and admin curl', () => {
  const text = formatProvisioningSummary({
    relayDashboardUrl: 'https://relay.example.workers.dev/d/uuid',
    deviceToken: 'tok',
    adminKey: 'adm',
    workerUrl: 'https://relay.example.workers.dev',
  })
  expect(text).toContain('https://relay.example.workers.dev/d/uuid')
  expect(text).toContain('tok')
  expect(text).toContain('Bearer adm')
})
