import { DurableObject } from 'cloudflare:workers'

export interface Env {
  DASHBOARD_RELAY: DurableObjectNamespace<DashboardRelay>
  RELAY_PUBLISH_KEY?: string
  ADMIN_KEY?: string
  DEVICE_TOKENS?: string
  DEVICE_UUID?: string
  DEVICE_TOKEN?: string
}

type DashboardPayload = Record<string, unknown>

type DashboardFrame =
  | { type: 'dashboard'; payload: DashboardPayload }
  | { type: 'ping' }

type StoredDashboard = {
  payload: DashboardPayload
  lastPublishAt: string
}

type DeviceStats = {
  uuid: string
  connected: boolean
  lastPublishAt: string | null
  publishCount: number
  lastFetchAt: string | null
  fetchCount: number
}

const JSON_HEADERS = {
  'content-type': 'application/json; charset=utf-8',
}

const STALE_AFTER_MS = 2 * 60 * 1000
const MAX_FRAME_BYTES = 16 * 1024
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i

function jsonResponse(body: unknown, init: ResponseInit = {}): Response {
  return new Response(JSON.stringify(body), {
    ...init,
    headers: {
      ...JSON_HEADERS,
      ...init.headers,
    },
  })
}

function textResponse(body: string, status = 200): Response {
  return new Response(body, { status, headers: { 'content-type': 'text/plain; charset=utf-8' } })
}

function bearerToken(request: Request): string {
  const auth = request.headers.get('authorization') ?? ''
  const match = /^Bearer\s+(.+)$/i.exec(auth)
  return match?.[1] ?? ''
}

function timingSafeEqual(a: string, b: string): boolean {
  const enc = new TextEncoder()
  const left = enc.encode(a)
  const right = enc.encode(b)
  const len = Math.max(left.length, right.length)
  let diff = left.length ^ right.length
  for (let i = 0; i < len; i++) {
    diff |= (left[i] ?? 0) ^ (right[i] ?? 0)
  }
  return diff === 0
}

function authorized(request: Request, expected?: string): boolean {
  return !!expected && timingSafeEqual(bearerToken(request), expected)
}

function parseDeviceTokens(env: Env): Record<string, string> {
  const configured = env.DEVICE_TOKENS?.trim()
  if (configured) {
    try {
      const parsed: unknown = JSON.parse(configured)
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        return Object.fromEntries(
          Object.entries(parsed)
            .filter((entry): entry is [string, string] => typeof entry[1] === 'string'),
        )
      }
    } catch {
      return {}
    }
  }
  if (env.DEVICE_UUID && env.DEVICE_TOKEN) {
    return { [env.DEVICE_UUID]: env.DEVICE_TOKEN }
  }
  return {}
}

function knownUuids(env: Env): string[] {
  return Object.keys(parseDeviceTokens(env)).filter((uuid) => UUID_RE.test(uuid))
}

function configuredDeviceToken(env: Env, uuid: string): string | undefined {
  return parseDeviceTokens(env)[uuid]
}

function relayStub(env: Env, uuid: string): DurableObjectStub<DashboardRelay> {
  const id = env.DASHBOARD_RELAY.idFromName(uuid)
  return env.DASHBOARD_RELAY.get(id)
}

function connectRequest(request: Request, env: Env, url: URL): Response | Promise<Response> {
  if (request.headers.get('upgrade')?.toLowerCase() !== 'websocket') {
    return textResponse('Expected WebSocket upgrade', 426)
  }
  if (!authorized(request, env.RELAY_PUBLISH_KEY)) {
    return jsonResponse({ error: 'Unauthorized' }, { status: 401 })
  }

  const uuid = url.searchParams.get('uuid') ?? ''
  if (!UUID_RE.test(uuid)) {
    return jsonResponse({ error: 'Invalid uuid' }, { status: 400 })
  }

  return relayStub(env, uuid).fetch(request)
}

function dashboardRequest(request: Request, env: Env, uuid: string): Promise<Response> | Response {
  if (!UUID_RE.test(uuid)) {
    return jsonResponse({ error: 'Invalid uuid' }, { status: 400 })
  }

  const headers = new Headers(request.headers)
  headers.delete('x-relay-device-token')
  headers.delete('x-relay-stale-after-ms')
  headers.delete('x-relay-uuid')
  const token = configuredDeviceToken(env, uuid)
  if (token) headers.set('x-relay-device-token', token)
  headers.set('x-relay-stale-after-ms', String(STALE_AFTER_MS))
  headers.set('x-relay-uuid', uuid)

  return relayStub(env, uuid).fetch(new Request(request, { headers }))
}

async function adminStats(request: Request, env: Env, url: URL): Promise<Response> {
  if (!authorized(request, env.ADMIN_KEY)) {
    return jsonResponse({ error: 'Unauthorized' }, { status: 401 })
  }

  const requestedUuid = url.searchParams.get('uuid')
  const uuids = requestedUuid ? [requestedUuid] : knownUuids(env)
  if (uuids.length === 0) {
    return jsonResponse({ devices: [] })
  }

  const devices = await Promise.all(uuids.map(async (uuid) => {
    if (!UUID_RE.test(uuid)) return null
    const res = await relayStub(env, uuid).fetch('https://relay.internal/admin/stats', {
      headers: { 'x-relay-uuid': uuid },
    })
    if (!res.ok) return null
    return await res.json() as DeviceStats
  }))

  return jsonResponse({ devices: devices.filter((device): device is DeviceStats => device !== null) })
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url)

    if (url.pathname === '/health') {
      return jsonResponse({ ok: true })
    }

    if (url.pathname === '/connect') {
      return connectRequest(request, env, url)
    }

    if (url.pathname === '/admin/stats') {
      return adminStats(request, env, url)
    }

    const dashboardMatch = /^\/d\/([^/]+)(?:\/dashboard)?$/.exec(url.pathname)
    if (dashboardMatch) {
      return dashboardRequest(request, env, dashboardMatch[1])
    }

    return jsonResponse({ error: 'Not found' }, { status: 404 })
  },
}

export class DashboardRelay extends DurableObject<Env> {
  constructor(
    private readonly relayState: DurableObjectState,
    relayEnv: Env,
  ) {
    super(relayState, relayEnv)
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url)

    if (request.headers.get('upgrade')?.toLowerCase() === 'websocket') {
      await this.noteUuid(new URL(request.url).searchParams.get('uuid') ?? request.headers.get('x-relay-uuid'))
      return this.acceptPublisher()
    }

    if (url.pathname === '/admin/stats') {
      await this.noteUuid(request.headers.get('x-relay-uuid'))
      return jsonResponse(await this.stats())
    }

    await this.noteUuid(request.headers.get('x-relay-uuid'))
    if (!await this.authorizeDevice(request)) {
      return jsonResponse({ error: 'Unauthorized' }, { status: 401 })
    }

    const stored = await this.relayState.storage.get<StoredDashboard>('dashboard')
    if (!stored) {
      return jsonResponse({ error: 'Dashboard not published yet' }, { status: 404 })
    }

    await this.recordFetch()
    const lastPublish = Date.parse(stored.lastPublishAt)
    const staleAfterMs = Number(request.headers.get('x-relay-stale-after-ms') ?? STALE_AFTER_MS)
    const stale = Number.isFinite(lastPublish) && Date.now() - lastPublish > staleAfterMs

    return jsonResponse({
      ...stored.payload,
      stale,
    })
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const text = typeof message === 'string' ? message : new TextDecoder().decode(message)
    if (text.length > MAX_FRAME_BYTES) {
      ws.send(JSON.stringify({ type: 'error', error: 'frame_too_large' }))
      return
    }

    let frame: DashboardFrame
    try {
      frame = JSON.parse(text) as DashboardFrame
    } catch {
      ws.send(JSON.stringify({ type: 'error', error: 'invalid_json' }))
      return
    }

    if (frame.type === 'ping') {
      ws.send(JSON.stringify({ type: 'pong' }))
      return
    }

    if (frame.type !== 'dashboard' || !frame.payload || typeof frame.payload !== 'object' || Array.isArray(frame.payload)) {
      ws.send(JSON.stringify({ type: 'error', error: 'invalid_frame' }))
      return
    }

    await this.storeDashboard(frame.payload)
    ws.send(JSON.stringify({ type: 'ack' }))
  }

  webSocketClose(): void {}

  webSocketError(): void {}

  private acceptPublisher(): Response {
    const pair = new WebSocketPair()
    const [client, server] = Object.values(pair)
    this.relayState.acceptWebSocket(server)
    return new Response(null, { status: 101, webSocket: client })
  }

  private async authorizeDevice(request: Request): Promise<boolean> {
    const expected = request.headers.get('x-relay-device-token') ?? undefined
    return authorized(request, expected)
  }

  private async storeDashboard(payload: DashboardPayload): Promise<void> {
    const now = new Date().toISOString()
    await this.relayState.storage.put('dashboard', { payload, lastPublishAt: now } satisfies StoredDashboard)
    await this.relayState.storage.put('lastPublishAt', now)
    await this.increment('publishCount')
  }

  private async recordFetch(): Promise<void> {
    await this.relayState.storage.put('lastFetchAt', new Date().toISOString())
    await this.increment('fetchCount')
  }

  private async increment(key: string): Promise<void> {
    const current = await this.relayState.storage.get<number>(key)
    await this.relayState.storage.put(key, (current ?? 0) + 1)
  }

  private async noteUuid(uuid: string | null): Promise<void> {
    if (uuid && UUID_RE.test(uuid)) {
      await this.relayState.storage.put('uuid', uuid)
    }
  }

  private async stats(): Promise<DeviceStats> {
    const uuid = await this.relayState.storage.get<string>('uuid')
    return {
      uuid: uuid ?? this.relayState.id.toString(),
      connected: this.relayState.getWebSockets().length > 0,
      lastPublishAt: await this.relayState.storage.get<string>('lastPublishAt') ?? null,
      publishCount: await this.relayState.storage.get<number>('publishCount') ?? 0,
      lastFetchAt: await this.relayState.storage.get<string>('lastFetchAt') ?? null,
      fetchCount: await this.relayState.storage.get<number>('fetchCount') ?? 0,
    }
  }
}
