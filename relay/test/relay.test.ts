import { exports } from 'cloudflare:workers'
import { test, expect } from 'vitest'

const UUID = '11111111-1111-4111-8111-111111111111'
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

  const response = await worker.fetch(`https://relay.test/d/${UUID}`, {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(200)
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
  const response = await worker.fetch('https://relay.test/d/22222222-2222-4222-8222-222222222222', {
    headers: DEVICE_AUTH,
  })
  expect(response.status).toBe(401)
})
