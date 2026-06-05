import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  buildRelayConnectUrl,
  createRelayPublisher,
  relayConfigFromEnv,
  validateRelayConfig,
  type RelayConfig,
} from './relay-client.js'

test('relayConfigFromEnv defaults to disabled', () => {
  assert.deepEqual(relayConfigFromEnv({}), {
    enabled: false,
    relayUrl: '',
    publishKey: '',
    deviceUuid: '',
    publishIntervalMs: 300_000,
    reconnectMinMs: 1_000,
    reconnectMaxMs: 60_000,
  })
})

test('validateRelayConfig reports required fields only when enabled', () => {
  assert.deepEqual(validateRelayConfig(relayConfigFromEnv({})), [])
  assert.deepEqual(
    validateRelayConfig(relayConfigFromEnv({ RELAY_ENABLED: 'true' })),
    ['RELAY_URL', 'RELAY_PUBLISH_KEY', 'DEVICE_UUID'],
  )
})

test('buildRelayConnectUrl converts worker base URL to websocket connect URL', () => {
  assert.equal(
    buildRelayConnectUrl('https://relay.example.com', '11111111-1111-4111-8111-111111111111'),
    'wss://relay.example.com/connect?uuid=11111111-1111-4111-8111-111111111111',
  )
})

test('buildRelayConnectUrl accepts local ws URLs', () => {
  assert.equal(
    buildRelayConnectUrl('ws://127.0.0.1:8787/anything', '11111111-1111-4111-8111-111111111111'),
    'ws://127.0.0.1:8787/connect?uuid=11111111-1111-4111-8111-111111111111',
  )
})

test('enabled-but-incomplete relay config opens no socket and does not throw', () => {
  // RELAY_ENABLED=true with missing URL/key/uuid must not crash the API: the
  // publisher logs the gap and stays silent, leaving the direct route intact.
  const config: RelayConfig = {
    enabled: true,
    relayUrl: '',
    publishKey: '',
    deviceUuid: '',
    publishIntervalMs: 300_000,
    reconnectMinMs: 1_000,
    reconnectMaxMs: 60_000,
  }
  const warnings: unknown[] = []
  const logger = {
    info() {},
    warn(obj: unknown) { warnings.push(obj) },
    error() {},
  }

  const publisher = createRelayPublisher({
    config,
    logger,
    getPayload: async () => { throw new Error('getPayload must not be called') },
  })

  assert.doesNotThrow(() => publisher.start())
  assert.deepEqual(warnings, [{ missing: ['RELAY_URL', 'RELAY_PUBLISH_KEY', 'DEVICE_UUID'] }])
  publisher.stop()
})
