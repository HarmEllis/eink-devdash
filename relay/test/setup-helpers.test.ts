import { test, expect } from 'vitest'

import {
  base64url,
  buildLoginArgs,
  buildRelayDashboardUrl,
  chooseAuthMode,
  chooseIdentityMode,
  classifyEnvIdentity,
  formatEnvBlock,
  formatProvisioningSummary,
  generateIdentity,
  generateToken,
  identityFromEnv,
  mergeEnv,
  normalizeIdentityOverride,
  normalizeReuseAnswer,
  normalizeYesNo,
  parseWorkerUrl,
  relayEnvUpdates,
  workerIdentitySecret,
  workerSecretName,
} from '../scripts/setup-helpers.mjs'

const COMPLETE_ENV = [
  'DEVICE_UUID=11111111-1111-4111-8111-111111111111',
  'DEVICE_TOKEN=tok',
  'RELAY_PUBLISH_KEY=pub',
].join('\n')

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

test('identityFromEnv reuses a complete identity and admin key', () => {
  expect(identityFromEnv([
    `DEVICE_UUID=11111111-1111-4111-8111-111111111111`,
    'DEVICE_TOKEN=tok',
    'RELAY_PUBLISH_KEY=pub',
    'RELAY_ADMIN_KEY=adm',
  ].join('\n'))).toEqual({
    deviceUuid: '11111111-1111-4111-8111-111111111111',
    deviceToken: 'tok',
    relayPublishKey: 'pub',
    adminKey: 'adm',
  })
})

test('identityFromEnv upgrades a legacy identity with a new admin key', () => {
  const identity = identityFromEnv([
    `DEVICE_UUID=11111111-1111-4111-8111-111111111111`,
    'DEVICE_TOKEN=tok',
    'RELAY_PUBLISH_KEY=pub',
  ].join('\n'))
  expect(identity).toMatchObject({
    deviceUuid: '11111111-1111-4111-8111-111111111111',
    deviceToken: 'tok',
    relayPublishKey: 'pub',
  })
  expect(identity?.adminKey).toHaveLength(43)
})

test('identityFromEnv rejects a partial existing identity', () => {
  expect(() => identityFromEnv('DEVICE_UUID=11111111-1111-4111-8111-111111111111\n'))
    .toThrow(/incomplete relay identity/)
})

test('identityFromEnv ignores a direct-only API token', () => {
  expect(identityFromEnv('DEVICE_TOKEN=direct-api-token\n')).toBeNull()
})

test('classifyEnvIdentity distinguishes none/complete/partial', () => {
  expect(classifyEnvIdentity('')).toBe('none')
  // A lone direct-API token is not a relay identity.
  expect(classifyEnvIdentity('DEVICE_TOKEN=direct-api-token\n')).toBe('none')
  expect(classifyEnvIdentity(COMPLETE_ENV)).toBe('complete')
  // Missing RELAY_PUBLISH_KEY.
  expect(classifyEnvIdentity('DEVICE_UUID=11111111-1111-4111-8111-111111111111\n')).toBe('partial')
  // All three present but the UUID is invalid.
  expect(classifyEnvIdentity([
    'DEVICE_UUID=not-a-uuid',
    'DEVICE_TOKEN=tok',
    'RELAY_PUBLISH_KEY=pub',
  ].join('\n'))).toBe('partial')
})

test('normalizeIdentityOverride accepts reuse/new, rejects junk', () => {
  expect(normalizeIdentityOverride(undefined)).toBeUndefined()
  expect(normalizeIdentityOverride('')).toBeUndefined()
  expect(normalizeIdentityOverride(' Reuse ')).toBe('reuse')
  expect(normalizeIdentityOverride('NEW')).toBe('new')
  expect(() => normalizeIdentityOverride('rotate')).toThrow(/must be "reuse" or "new"/)
})

test('chooseIdentityMode: no identity always generates', () => {
  expect(chooseIdentityMode({ existing: 'none', override: undefined, isInteractive: true })).toBe('new')
  expect(chooseIdentityMode({ existing: 'none', override: undefined, isInteractive: false })).toBe('new')
})

test('chooseIdentityMode: complete identity prompts on a TTY, reuses without one', () => {
  expect(chooseIdentityMode({ existing: 'complete', override: undefined, isInteractive: true }))
    .toBe('prompt-reuse')
  expect(chooseIdentityMode({ existing: 'complete', override: undefined, isInteractive: false }))
    .toBe('reuse')
})

test('chooseIdentityMode: explicit override wins over prompting', () => {
  expect(chooseIdentityMode({ existing: 'complete', override: 'new', isInteractive: true })).toBe('new')
  expect(chooseIdentityMode({ existing: 'complete', override: 'reuse', isInteractive: false })).toBe('reuse')
})

test('chooseIdentityMode: reuse override without a complete identity throws', () => {
  expect(() => chooseIdentityMode({ existing: 'none', override: 'reuse', isInteractive: false }))
    .toThrow(/no complete identity/)
  expect(() => chooseIdentityMode({ existing: 'partial', override: 'reuse', isInteractive: true }))
    .toThrow(/no complete identity/)
})

test('chooseIdentityMode: partial offers new on a TTY, fails loudly otherwise', () => {
  expect(chooseIdentityMode({ existing: 'partial', override: undefined, isInteractive: true }))
    .toBe('prompt-new')
  expect(() => chooseIdentityMode({ existing: 'partial', override: undefined, isInteractive: false }))
    .toThrow(/incomplete relay identity/)
})

test('normalizeReuseAnswer: bare Enter reuses, recognizes r/n synonyms', () => {
  for (const yes of ['', 'r', 'reuse', 'R', ' yes ', 'y']) {
    expect(normalizeReuseAnswer(yes)).toBe('reuse')
  }
  for (const no of ['n', 'new', 'NO']) {
    expect(normalizeReuseAnswer(no)).toBe('new')
  }
  expect(normalizeReuseAnswer('maybe')).toBeNull()
})

test('normalizeYesNo: bare Enter is yes, recognizes y/n synonyms', () => {
  for (const yes of ['', 'y', 'Yes', ' y ']) {
    expect(normalizeYesNo(yes)).toBe('yes')
  }
  for (const no of ['n', 'NO', ' no ']) {
    expect(normalizeYesNo(no)).toBe('no')
  }
  expect(normalizeYesNo('huh')).toBeNull()
})

test('worker identity uses one isolated secret binding per uuid', () => {
  expect(workerSecretName('11111111-1111-4111-8111-111111111111'))
    .toBe('DEVICE_IDENTITY_11111111111141118111111111111111')
  expect(workerIdentitySecret({
    deviceToken: 'tok',
    relayPublishKey: 'pub',
    adminKey: 'adm',
  })).toBe('{"deviceToken":"tok","publishKey":"pub","adminKey":"adm"}')
})

test('relayEnvUpdates includes DEVICE_TOKEN so the API can start', () => {
  const updates = relayEnvUpdates({
    deviceUuid: 'uuid',
    deviceToken: 'tok',
    relayPublishKey: 'pub',
    adminKey: 'adm',
    workerUrl: 'https://relay.example.workers.dev',
  })
  expect(updates).toEqual({
    DEVICE_TOKEN: 'tok',
    DEVICE_UUID: 'uuid',
    RELAY_ENABLED: 'true',
    RELAY_URL: 'https://relay.example.workers.dev',
    RELAY_PUBLISH_KEY: 'pub',
    RELAY_ADMIN_KEY: 'adm',
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

test('formatEnvBlock emits all relay keys as KEY=VALUE lines', () => {
  const block = formatEnvBlock(
    { deviceUuid: 'uuid', deviceToken: 'tok', relayPublishKey: 'pub', adminKey: 'adm' },
    'https://relay.example.workers.dev',
  )
  expect(block).toContain('DEVICE_TOKEN=tok')
  expect(block).toContain('DEVICE_UUID=uuid')
  expect(block).toContain('RELAY_ENABLED=true')
  expect(block).toContain('RELAY_URL=https://relay.example.workers.dev')
  expect(block).toContain('RELAY_PUBLISH_KEY=pub')
  expect(block).toContain('RELAY_ADMIN_KEY=adm')
})

test('chooseAuthMode prefers a token, then a TTY, else unavailable', () => {
  expect(chooseAuthMode({ hasToken: true, isInteractive: false })).toBe('token')
  expect(chooseAuthMode({ hasToken: true, isInteractive: true })).toBe('token')
  expect(chooseAuthMode({ hasToken: false, isInteractive: true })).toBe('login')
  expect(chooseAuthMode({ hasToken: false, isInteractive: false })).toBe('unavailable')
})

test('buildLoginArgs stays bare locally and adds container flags with a callback host', () => {
  expect(buildLoginArgs()).toEqual(['login'])
  expect(buildLoginArgs({ callbackHost: '0.0.0.0' })).toEqual([
    'login',
    '--browser=false',
    '--callback-host=0.0.0.0',
  ])
  expect(buildLoginArgs({ callbackHost: '0.0.0.0', callbackPort: '8976' })).toEqual([
    'login',
    '--browser=false',
    '--callback-host=0.0.0.0',
    '--callback-port=8976',
  ])
})
