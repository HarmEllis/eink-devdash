import { exports } from 'cloudflare:workers'
import { test, expect } from 'vitest'

const UUID = '11111111-1111-4111-8111-111111111111'
const UUID_COALESCE = '22222222-2222-4222-8222-222222222222'
const UUID_LEGACY = '33333333-3333-4333-8333-333333333333'
const UUID_FAILOVER = '44444444-4444-4444-8444-444444444444'
const UUID_ERROR = '55555555-5555-4555-8555-555555555555'
const PUBLISH_AUTH = { authorization: 'Bearer publish-test' }
const DEVICE_AUTH = { authorization: 'Bearer device-test' }
const ADMIN_AUTH = { authorization: 'Bearer admin-test' }
const worker = exports.default as {
  fetch(request: Request | string, init?: RequestInit): Promise<Response>
}

async function connect(uuid = UUID): Promise<WebSocket> {
  const request = new Request(`https://relay.test/connect?uuid=${uuid}`, {
    headers: {
      ...PUBLISH_AUTH,
      upgrade: 'websocket',
    },
  })
  const response = await worker.fetch(request)
  const ws = response.webSocket
  if (!ws) throw new Error('missing websocket')
  ws.accept()
  return ws
}

function nextMessage(ws: WebSocket): Promise<MessageEvent> {
  return new Promise((resolve) => ws.addEventListener('message', resolve, { once: true }))
}

async function advertise(ws: WebSocket, capabilities: string[]) {
  ws.send(JSON.stringify({ type: 'hello', capabilities }))
  const pong = nextMessage(ws)
  ws.send(JSON.stringify({ type: 'ping' }))
  await expect(pong).resolves.toMatchObject({ data: '{"type":"pong"}' })
}

async function pushDashboard(ws: WebSocket, updatedAt: string) {
  const ack = nextMessage(ws)
  ws.send(JSON.stringify({
    type: 'dashboard',
    payload: {
      schemaVersion: 2,
      services: [],
      updatedAt,
      updatedAtLocal: '12:00',
      updatedAtLocalIso: '2026-06-07T12:00:00',
    },
  }))
  await expect(ack).resolves.toMatchObject({ data: '{"type":"ack"}' })
}

test('health returns ok without auth', async () => {
  const response = await worker.fetch('https://relay.test/health')
  expect(response.status).toBe(200)
  await expect(response.json()).resolves.toEqual({ ok: true })
})

test('rejects unauthenticated publisher websocket', async () => {
  const response = await worker.fetch(`https://relay.test/connect?uuid=${UUID}`, {
    headers: { upgrade: 'websocket' },
  })
  expect(response.status).toBe(401)
})

test('publishes dashboard over websocket and serves it to the device', async () => {
  const ws = await connect()
  ws.send(JSON.stringify({ type: 'hello', capabilities: ['dashboard'] }))
  const message = nextMessage(ws)
  ws.send(JSON.stringify({
    type: 'dashboard',
    payload: {
      schemaVersion: 2,
      services: [],
      updatedAt: '2026-06-05T10:00:00.000Z',
      updatedAtLocal: '12:00',
      updatedAtLocalIso: '2026-06-05T12:00:00',
    },
  }))
  await expect(message).resolves.toMatchObject({ data: '{"type":"ack"}' })
  ws.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string; id?: string; resource?: string }
    if (frame.type === 'request' && frame.id && frame.resource === 'dashboard') {
      ws.send(JSON.stringify({ type: 'request-ack', id: frame.id }))
      ws.send(JSON.stringify({
        type: 'response',
        id: frame.id,
        resource: 'dashboard',
        payload: {
          schemaVersion: 2,
          services: [],
          updatedAt: '2026-06-05T10:00:00.000Z',
          updatedAtLocal: '12:00',
          updatedAtLocalIso: '2026-06-05T12:00:00',
        },
      }))
    }
  })

  const response = await worker.fetch(`https://relay.test/d/${UUID}`, {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(200)
  expect(response.headers.get('cache-control')).toBe('no-store')
  expect(response.headers.get('x-relay-response-nonce')).toBeTruthy()
  await expect(response.json()).resolves.toEqual({
    schemaVersion: 2,
    services: [],
    updatedAt: '2026-06-05T10:00:00.000Z',
    updatedAtLocal: '12:00',
    updatedAtLocalIso: '2026-06-05T12:00:00',
    stale: false,
  })
  ws.close()
})

test('serves OTA manifest on demand and never stores a fallback copy', async () => {
  const ws = await connect()
  ws.send(JSON.stringify({ type: 'hello', capabilities: ['manifest'] }))
  ws.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string; id?: string; resource?: string }
    if (frame.type === 'request' && frame.id && frame.resource === 'manifest') {
      ws.send(JSON.stringify({ type: 'request-ack', id: frame.id }))
      ws.send(JSON.stringify({
        type: 'response',
        id: frame.id,
        resource: 'manifest',
        payload: {
          otaEnabled: true,
          latestVersion: 'v0.4.0',
          downloadUrl:
            'https://github.com/HarmEllis/eink-devdash/releases/download/v0.4.0/eink-devdash.bin',
        },
      }))
    }
  })
  const fresh = await worker.fetch(`https://relay.test/d/${UUID}/ota/manifest`, {
    headers: DEVICE_AUTH,
  })
  expect(fresh.status).toBe(200)
  expect(fresh.headers.get('cache-control')).toBe('no-store')
  ws.close()

  const unavailable = await worker.fetch(`https://relay.test/d/${UUID}/ota/manifest`, {
    headers: DEVICE_AUTH,
  })
  expect(unavailable.status).toBe(404)
  expect(unavailable.headers.get('cache-control')).toBe('no-store')
})

test('coalesces concurrent dashboard fetches into one publisher request', async () => {
  const ws = await connect(UUID_COALESCE)
  await advertise(ws, ['dashboard'])
  let requests = 0
  ws.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string; id?: string; resource?: string }
    if (frame.type !== 'request' || frame.resource !== 'dashboard' || !frame.id) return
    requests += 1
    ws.send(JSON.stringify({ type: 'request-ack', id: frame.id }))
    setTimeout(() => ws.send(JSON.stringify({
      type: 'response',
      id: frame.id,
      resource: 'dashboard',
      payload: {
        schemaVersion: 2,
        services: [],
        updatedAt: '2026-06-07T10:10:00.000Z',
        updatedAtLocal: '12:10',
        updatedAtLocalIso: '2026-06-07T12:10:00',
      },
    })), 20)
  })

  const responses = await Promise.all(Array.from({ length: 4 }, () => worker.fetch(
    `https://relay.test/d/${UUID_COALESCE}`,
    { headers: DEVICE_AUTH },
  )))
  expect(requests).toBe(1)
  expect(responses.every((response) => response.status === 200)).toBe(true)
  const bodies = await Promise.all(responses.map((response) => response.json()))
  expect(bodies.every((body) => (body as { stale: boolean }).stale === false)).toBe(true)
  ws.close()
})

test('legacy publisher is not sent requests and dashboard fallback is immediate', async () => {
  const legacy = await connect(UUID_LEGACY)
  let receivedRequest = false
  legacy.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string }
    if (frame.type === 'request') receivedRequest = true
  })
  const started = Date.now()
  const response = await worker.fetch(`https://relay.test/d/${UUID_LEGACY}`, {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(404)
  expect(Date.now() - started).toBeLessThan(100)
  expect(receivedRequest).toBe(false)
  expect(await response.json()).toMatchObject({ error: 'Dashboard not published yet' })
  legacy.close()
})

test('unacknowledged newest publisher fails over with the same request id', async () => {
  const older = await connect(UUID_FAILOVER)
  await advertise(older, ['dashboard'])
  const newer = await connect(UUID_FAILOVER)
  await advertise(newer, ['dashboard'])
  let newerId = ''
  let olderId = ''
  newer.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string; id?: string }
    if (frame.type === 'request' && frame.id) newerId = frame.id
  })
  older.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string; id?: string; resource?: string }
    if (frame.type !== 'request' || !frame.id) return
    olderId = frame.id
    older.send(JSON.stringify({ type: 'request-ack', id: frame.id }))
    older.send(JSON.stringify({
      type: 'response',
      id: frame.id,
      resource: 'dashboard',
      payload: {
        schemaVersion: 2,
        services: [],
        updatedAt: '2026-06-07T10:30:00.000Z',
        updatedAtLocal: '12:30',
        updatedAtLocalIso: '2026-06-07T12:30:00',
      },
    }))
  })

  const response = await worker.fetch(`https://relay.test/d/${UUID_FAILOVER}`, {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(200)
  expect(newerId).toBeTruthy()
  expect(olderId).toBe(newerId)
  expect(await response.json()).toMatchObject({ stale: false })
  older.close()
  newer.close()
})

test('publisher error response falls back immediately to the stored dashboard', async () => {
  const ws = await connect(UUID_ERROR)
  await advertise(ws, ['dashboard'])
  await pushDashboard(ws, '2026-06-07T10:40:00.000Z')
  ws.addEventListener('message', (event) => {
    const frame = JSON.parse(String(event.data)) as { type: string; id?: string }
    if (frame.type !== 'request' || !frame.id) return
    ws.send(JSON.stringify({ type: 'request-ack', id: frame.id }))
    ws.send(JSON.stringify({
      type: 'response',
      id: frame.id,
      resource: 'dashboard',
      error: 'build_failed',
    }))
  })
  const started = Date.now()
  const response = await worker.fetch(`https://relay.test/d/${UUID_ERROR}`, {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(200)
  expect(Date.now() - started).toBeLessThan(100)
  expect(await response.json()).toMatchObject({ stale: true })
  ws.close()
})

test('device fetch requires its bearer token', async () => {
  const response = await worker.fetch(`https://relay.test/d/${UUID}`)
  expect(response.status).toBe(401)
})

test('device fetch ignores client-supplied internal relay token headers', async () => {
  const response = await worker.fetch(`https://relay.test/d/${UUID}`, {
    headers: {
      authorization: 'Bearer wrong-device-token',
      'x-relay-device-token': 'device-test',
    },
  })
  expect(response.status).toBe(401)
})

test('unknown uuid cannot be authorized by spoofed internal token header', async () => {
  const unknownUuid = '22222222-2222-4222-8222-222222222222'
  const ws = await connect(unknownUuid)
  const message = nextMessage(ws)
  ws.send(JSON.stringify({
    type: 'dashboard',
    payload: {
      schemaVersion: 2,
      services: [],
      updatedAt: '2026-06-05T10:00:00.000Z',
      updatedAtLocal: '12:00',
      updatedAtLocalIso: '2026-06-05T12:00:00',
    },
  }))
  await expect(message).resolves.toMatchObject({ data: '{"type":"ack"}' })

  const response = await worker.fetch(`https://relay.test/d/${unknownUuid}`, {
    headers: {
      authorization: 'Bearer attacker-token',
      'x-relay-device-token': 'attacker-token',
    },
  })
  expect(response.status).toBe(401)
  ws.close()
})

test('admin stats reports publish and fetch counters', async () => {
  const response = await worker.fetch(`https://relay.test/admin/stats?uuid=${UUID}`, {
    headers: ADMIN_AUTH,
  })
  expect(response.status).toBe(200)
  const body = await response.json() as {
    devices: Array<{ publishCount: number; fetchCount: number }>
  }
  expect(body.devices).toHaveLength(1)
  expect(body.devices[0].publishCount).toBeGreaterThanOrEqual(1)
  expect(body.devices[0].fetchCount).toBeGreaterThanOrEqual(1)
})

test('unknown uuid without configured token is unauthorized', async () => {
  const response = await worker.fetch('https://relay.test/d/66666666-6666-4666-8666-666666666666', {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(401)
})
