import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  buildRelayConnectUrl,
  relayConfigFromEnv,
  validateRelayConfig,
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
