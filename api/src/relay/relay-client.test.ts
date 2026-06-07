import { test } from 'node:test'
import assert from 'node:assert/strict'
import { once } from 'node:events'
import { WebSocketServer, type WebSocket } from 'ws'

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

function jsonQueue(socket: WebSocket) {
  const frames: Array<Record<string, any>> = []
  const waiters: Array<(frame: Record<string, any>) => void> = []
  socket.on('message', (data) => {
    const frame = JSON.parse(data.toString()) as Record<string, any>
    const waiter = waiters.shift()
    if (waiter) waiter(frame)
    else frames.push(frame)
  })
  return {
    next(): Promise<Record<string, any>> {
      const frame = frames.shift()
      return frame ? Promise.resolve(frame) : new Promise((resolve) => waiters.push(resolve))
    },
  }
}

test('publisher negotiates capabilities and serves correlated requests', { timeout: 5_000 }, async () => {
  const server = new WebSocketServer({ host: '127.0.0.1', port: 0 })
  await once(server, 'listening')
  const address = server.address()
  if (!address || typeof address === 'string') throw new Error('missing websocket port')

  let payloadCalls = 0
  let resolveBuild!: (value: any) => void
  const publisher = createRelayPublisher({
    config: {
      enabled: true,
      relayUrl: `http://127.0.0.1:${address.port}`,
      publishKey: 'publish-test',
      deviceUuid: '11111111-1111-4111-8111-111111111111',
      publishIntervalMs: 60_000,
      reconnectMinMs: 5,
      reconnectMaxMs: 10,
    },
    logger: { info() {}, warn() {}, error() {} },
    getPayload: async (signal) => {
      assert.equal(signal, undefined)
      payloadCalls += 1
      if (payloadCalls === 1) {
        return {
          schemaVersion: 2,
          services: [],
          updatedAt: '2026-06-07T10:00:00.000Z',
          updatedAtLocal: '12:00',
          updatedAtLocalIso: '2026-06-07T12:00:00',
        }
      }
      return new Promise((resolve) => { resolveBuild = resolve })
    },
    getManifest: () => ({
      otaEnabled: true,
      latestVersion: 'v0.4.0',
      downloadUrl:
        'https://github.com/HarmEllis/eink-devdash/releases/download/v0.4.0/eink-devdash.bin',
    }),
  })

  try {
    const connected = new Promise<{ socket: WebSocket; messages: ReturnType<typeof jsonQueue> }>(
      (resolve) => server.once('connection', (socket) => {
        resolve({ socket, messages: jsonQueue(socket) })
      }),
    )
    publisher.start()
    const { socket, messages } = await connected

    assert.deepEqual(await messages.next(), {
      type: 'hello',
      capabilities: ['dashboard', 'manifest'],
    })
    assert.equal((await messages.next()).type, 'dashboard')

    socket.send(JSON.stringify({ type: 'request', id: 'dash-1', resource: 'dashboard' }))
    assert.deepEqual(await messages.next(), { type: 'request-ack', id: 'dash-1' })
    assert.equal(payloadCalls, 2)
    resolveBuild({
      schemaVersion: 2,
      services: [],
      updatedAt: '2026-06-07T10:01:00.000Z',
      updatedAtLocal: '12:01',
      updatedAtLocalIso: '2026-06-07T12:01:00',
    })
    assert.deepEqual(await messages.next(), {
      type: 'response',
      id: 'dash-1',
      resource: 'dashboard',
      payload: {
        schemaVersion: 2,
        services: [],
        updatedAt: '2026-06-07T10:01:00.000Z',
        updatedAtLocal: '12:01',
        updatedAtLocalIso: '2026-06-07T12:01:00',
      },
    })

    socket.send(JSON.stringify({ type: 'request', id: 'manifest-1', resource: 'manifest' }))
    assert.deepEqual(await messages.next(), { type: 'request-ack', id: 'manifest-1' })
    assert.deepEqual(await messages.next(), {
      type: 'response',
      id: 'manifest-1',
      resource: 'manifest',
      payload: {
        otaEnabled: true,
        latestVersion: 'v0.4.0',
        downloadUrl:
          'https://github.com/HarmEllis/eink-devdash/releases/download/v0.4.0/eink-devdash.bin',
      },
    })
  } finally {
    publisher.stop()
    for (const client of server.clients) client.terminate()
    await new Promise<void>((resolve) => server.close(() => resolve()))
  }
})
