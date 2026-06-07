import { DurableObject } from 'cloudflare:workers'

export interface Env {
  DASHBOARD_RELAY: DurableObjectNamespace<DashboardRelay>
  RELAY_PUBLISH_KEY?: string
  ADMIN_KEY?: string
  DEVICE_TOKENS?: string
  DEVICE_UUID?: string
  DEVICE_TOKEN?: string
  MIN_REFRESH_MS?: string
  ACK_TIMEOUT_MS?: string
  DASHBOARD_RESPONSE_DEADLINE_MS?: string
  MANIFEST_RESPONSE_DEADLINE_MS?: string
}

type Resource = 'dashboard' | 'manifest'
type DashboardPayload = Record<string, unknown>
type ResponseFrame = {
  type: 'response'
  id: string
  resource: Resource
  payload?: DashboardPayload
  error?: unknown
}
type PublisherFrame =
  | { type: 'hello'; capabilities?: unknown }
  | { type: 'dashboard'; payload?: unknown }
  | { type: 'request-ack'; id?: unknown }
  | ResponseFrame
  | { type: 'ping' }

type StoredDashboard = {
  payload: DashboardPayload
  lastPublishAt: string
  updatedAt: string | null
}
type SocketAttachment = { epoch: number; caps: Resource[] }
type Settled =
  | { kind: 'fresh'; body: DashboardPayload }
  | { kind: 'snapshot'; body: DashboardPayload }
  | { kind: 'fallback' }
type Inflight = {
  id: string
  resource: Resource
  candidates: WebSocket[]
  epochs: number[]
  attempt: number
  generation: number
  socket: WebSocket
  acked: boolean
  ackTimer: ReturnType<typeof setTimeout> | null
  deadlineTimer: ReturnType<typeof setTimeout>
  deadlineAt: number
  promise: Promise<Settled>
  deliver: (frame: ResponseFrame | null) => void
  onAck: (generation: number, socket: WebSocket) => void
  failover: (generation: number, socket: WebSocket) => void
}
type DeviceStats = {
  uuid: string
  connected: boolean
  lastPublishAt: string | null
  publishCount: number
  lastFetchAt: string | null
  fetchCount: number
}

const JSON_HEADERS = { 'content-type': 'application/json; charset=utf-8' }
const DEVICE_HEADERS = { ...JSON_HEADERS, 'cache-control': 'no-store' }
const STALE_AFTER_MS = 2 * 60 * 1000
const DEFAULT_MIN_REFRESH_MS = 60_000
const DEFAULT_ACK_TIMEOUT_MS = 2_000
const DEFAULT_DEADLINES: Record<Resource, number> = {
  dashboard: 16_000,
  manifest: 6_000,
}
const MAX_FAILOVER = 2
const MAX_FRAME_BYTES = 16 * 1024
const MAX_CAPABILITIES = 8
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i

function positiveInt(value: string | undefined, fallback: number): number {
  const parsed = Number.parseInt(value ?? '', 10)
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback
}

function jsonResponse(body: unknown, init: ResponseInit = {}): Response {
  return new Response(JSON.stringify(body), {
    ...init,
    headers: { ...JSON_HEADERS, ...init.headers },
  })
}

function deviceResponse(body: unknown, init: ResponseInit = {}): Response {
  return new Response(JSON.stringify(body), {
    ...init,
    headers: {
      ...DEVICE_HEADERS,
      'x-relay-response-nonce': crypto.randomUUID(),
      ...init.headers,
    },
  })
}

function textResponse(body: string, status = 200): Response {
  return new Response(body, { status, headers: { 'content-type': 'text/plain; charset=utf-8' } })
}

function bearerToken(request: Request): string {
  return /^Bearer\s+(.+)$/i.exec(request.headers.get('authorization') ?? '')?.[1] ?? ''
}

function timingSafeEqual(a: string, b: string): boolean {
  const enc = new TextEncoder()
  const left = enc.encode(a)
  const right = enc.encode(b)
  const len = Math.max(left.length, right.length)
  let diff = left.length ^ right.length
  for (let i = 0; i < len; i++) diff |= (left[i] ?? 0) ^ (right[i] ?? 0)
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
          Object.entries(parsed).filter((entry): entry is [string, string] => typeof entry[1] === 'string'),
        )
      }
    } catch {
      return {}
    }
  }
  return env.DEVICE_UUID && env.DEVICE_TOKEN ? { [env.DEVICE_UUID]: env.DEVICE_TOKEN } : {}
}

function knownUuids(env: Env): string[] {
  return Object.keys(parseDeviceTokens(env)).filter((uuid) => UUID_RE.test(uuid))
}

function relayStub(env: Env, uuid: string): DurableObjectStub<DashboardRelay> {
  return env.DASHBOARD_RELAY.get(env.DASHBOARD_RELAY.idFromName(uuid))
}

function connectRequest(request: Request, env: Env, url: URL): Response | Promise<Response> {
  if (request.headers.get('upgrade')?.toLowerCase() !== 'websocket') {
    return textResponse('Expected WebSocket upgrade', 426)
  }
  if (!authorized(request, env.RELAY_PUBLISH_KEY)) {
    return jsonResponse({ error: 'Unauthorized' }, { status: 401 })
  }
  const uuid = url.searchParams.get('uuid') ?? ''
  if (!UUID_RE.test(uuid)) return jsonResponse({ error: 'Invalid uuid' }, { status: 400 })
  return relayStub(env, uuid).fetch(request)
}

function dashboardRequest(request: Request, env: Env, uuid: string): Promise<Response> | Response {
  if (!UUID_RE.test(uuid)) return deviceResponse({ error: 'Invalid uuid' }, { status: 400 })
  const headers = new Headers(request.headers)
  headers.delete('x-relay-device-token')
  headers.delete('x-relay-stale-after-ms')
  headers.delete('x-relay-uuid')
  const token = parseDeviceTokens(env)[uuid]
  if (token) headers.set('x-relay-device-token', token)
  headers.set('x-relay-stale-after-ms', String(STALE_AFTER_MS))
  headers.set('x-relay-uuid', uuid)
  return relayStub(env, uuid).fetch(new Request(request, { headers }))
}

async function adminStats(request: Request, env: Env, url: URL): Promise<Response> {
  if (!authorized(request, env.ADMIN_KEY)) return jsonResponse({ error: 'Unauthorized' }, { status: 401 })
  const requestedUuid = url.searchParams.get('uuid')
  const uuids = requestedUuid ? [requestedUuid] : knownUuids(env)
  const devices = await Promise.all(uuids.map(async (uuid) => {
    if (!UUID_RE.test(uuid)) return null
    const res = await relayStub(env, uuid).fetch('https://relay.internal/admin/stats', {
      headers: { 'x-relay-uuid': uuid },
    })
    return res.ok ? await res.json() as DeviceStats : null
  }))
  return jsonResponse({ devices: devices.filter((device): device is DeviceStats => device !== null) })
}

async function cacheBypassProbe(request: Request, env: Env, url: URL): Promise<Response> {
  if (!authorized(request, env.ADMIN_KEY)) return jsonResponse({ error: 'Unauthorized' }, { status: 401 })
  const uuid = url.searchParams.get('uuid') ?? ''
  if (!UUID_RE.test(uuid)) return jsonResponse({ error: 'Invalid uuid' }, { status: 400 })
  const token = parseDeviceTokens(env)[uuid]
  if (!token) return jsonResponse({ error: 'Unknown uuid' }, { status: 404 })
  const target = new URL(`/d/${uuid}/ota/manifest`, url)
  const first = await fetch(target, { headers: { authorization: `Bearer ${token}` } })
  const second = await fetch(target, { headers: { authorization: `Bearer ${token}` } })
  const firstNonce = first.headers.get('x-relay-response-nonce')
  const secondNonce = second.headers.get('x-relay-response-nonce')
  return jsonResponse({
    bypassed: !!firstNonce && !!secondNonce && firstNonce !== secondNonce,
    firstNonce,
    secondNonce,
    cacheControl: second.headers.get('cache-control'),
  })
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url)
    if (url.pathname === '/health') return jsonResponse({ ok: true })
    if (url.pathname === '/connect') return connectRequest(request, env, url)
    if (url.pathname === '/admin/stats') return adminStats(request, env, url)
    if (url.pathname === '/admin/cache-bypass-probe') return cacheBypassProbe(request, env, url)
    const match = /^\/d\/([^/]+)(?:\/dashboard|\/ota\/manifest)?$/.exec(url.pathname)
    if (match) return dashboardRequest(request, env, match[1])
    return jsonResponse({ error: 'Not found' }, { status: 404 })
  },
}

export class DashboardRelay extends DurableObject<Env> {
  private readonly inflight = new Map<Resource, Inflight>()
  private readonly minRefreshMs: number
  private readonly ackTimeoutMs: number
  private readonly deadlines: Record<Resource, number>

  constructor(
    private readonly relayState: DurableObjectState,
    relayEnv: Env,
  ) {
    super(relayState, relayEnv)
    this.minRefreshMs = positiveInt(relayEnv.MIN_REFRESH_MS, DEFAULT_MIN_REFRESH_MS)
    this.ackTimeoutMs = positiveInt(relayEnv.ACK_TIMEOUT_MS, DEFAULT_ACK_TIMEOUT_MS)
    this.deadlines = {
      dashboard: positiveInt(relayEnv.DASHBOARD_RESPONSE_DEADLINE_MS, DEFAULT_DEADLINES.dashboard),
      manifest: positiveInt(relayEnv.MANIFEST_RESPONSE_DEADLINE_MS, DEFAULT_DEADLINES.manifest),
    }
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url)
    if (request.headers.get('upgrade')?.toLowerCase() === 'websocket') {
      await this.noteUuid(url.searchParams.get('uuid') ?? request.headers.get('x-relay-uuid'))
      return this.acceptPublisher()
    }
    if (url.pathname === '/admin/stats') {
      await this.noteUuid(request.headers.get('x-relay-uuid'))
      return jsonResponse(await this.stats())
    }

    await this.noteUuid(request.headers.get('x-relay-uuid'))
    if (!await this.authorizeDevice(request)) {
      return deviceResponse({ error: 'Unauthorized' }, { status: 401 })
    }
    const resource: Resource = url.pathname.endsWith('/ota/manifest') ? 'manifest' : 'dashboard'
    let result: Settled
    const active = this.inflight.get(resource)
    if (active) {
      result = await active.promise
    } else {
      const nextAllowedAt = await this.relayState.storage.get<number>(`nextAllowedAt:${resource}`) ?? 0
      if (Date.now() < nextAllowedAt) {
        result = resource === 'dashboard'
          ? await this.snapshotResult(request, false)
          : { kind: 'fallback' }
      } else {
        const selected = await this.selectCandidates(resource)
        result = selected.sockets.length === 0
          ? { kind: 'fallback' }
          : await this.startRequest(resource, selected.sockets, selected.epochs)
      }
    }
    await this.recordFetch()
    return this.responseFor(resource, result, request)
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const text = typeof message === 'string' ? message : new TextDecoder().decode(message)
    if (text.length > MAX_FRAME_BYTES) {
      ws.send(JSON.stringify({ type: 'error', error: 'frame_too_large' }))
      return
    }
    let frame: PublisherFrame
    try {
      frame = JSON.parse(text) as PublisherFrame
    } catch {
      ws.send(JSON.stringify({ type: 'error', error: 'invalid_json' }))
      return
    }

    if (frame.type === 'ping') {
      ws.send(JSON.stringify({ type: 'pong' }))
      return
    }
    if (frame.type === 'hello') {
      const caps = this.parseCapabilities(frame.capabilities)
      if (caps) {
        const attachment = this.attachment(ws)
        ws.serializeAttachment({ epoch: attachment.epoch, caps } satisfies SocketAttachment)
      }
      return
    }
    if (frame.type === 'dashboard') {
      if (this.isPayload(frame.payload)) {
        const preferred = (await this.selectCandidates('dashboard')).sockets[0]
        if (preferred === ws) await this.storeDashboard(frame.payload)
      }
      ws.send(JSON.stringify({ type: 'ack' }))
      return
    }
    if (frame.type === 'request-ack' && typeof frame.id === 'string') {
      for (const entry of this.inflight.values()) {
        if (entry.id === frame.id && entry.socket === ws) {
          entry.onAck(entry.generation, ws)
          return
        }
      }
      return
    }
    if (
      frame.type === 'response'
      && typeof frame.id === 'string'
      && (frame.resource === 'dashboard' || frame.resource === 'manifest')
    ) {
      const entry = this.inflight.get(frame.resource)
      if (entry && entry.id === frame.id && entry.socket === ws) entry.deliver(frame)
      return
    }
    ws.send(JSON.stringify({ type: 'error', error: 'invalid_frame' }))
  }

  webSocketClose(ws: WebSocket): void {
    this.failInflightForSocket(ws)
  }

  webSocketError(ws: WebSocket): void {
    this.failInflightForSocket(ws)
  }

  private async acceptPublisher(): Promise<Response> {
    const pair = new WebSocketPair()
    const [client, server] = Object.values(pair)
    const previous = await this.relayState.storage.get<number>('publisherEpoch') ?? 0
    const epoch = previous + 1
    await this.relayState.storage.put('publisherEpoch', epoch)
    server.serializeAttachment({ epoch, caps: [] } satisfies SocketAttachment)
    this.relayState.acceptWebSocket(server)
    return new Response(null, { status: 101, webSocket: client })
  }

  private async startRequest(
    resource: Resource,
    candidates: WebSocket[],
    epochs: number[],
  ): Promise<Settled> {
    const id = crypto.randomUUID()
    let delivered = false
    let resolveFrame!: (frame: ResponseFrame | null) => void
    const deferred = new Promise<ResponseFrame | null>((resolve) => { resolveFrame = resolve })
    const deadlineAt = Date.now() + this.deadlines[resource]
    const entry = {} as Inflight
    const deliver = (frame: ResponseFrame | null) => {
      if (delivered) return
      delivered = true
      resolveFrame(frame)
    }
    const sendToCandidate = () => {
      if (delivered) return
      entry.generation += 1
      const generation = entry.generation
      const socket = entry.socket
      entry.acked = false
      try {
        if (socket.readyState !== WebSocket.OPEN) throw new Error('socket not open')
        socket.send(JSON.stringify({ type: 'request', id, resource }))
        entry.ackTimer = setTimeout(
          () => entry.failover(generation, socket),
          Math.min(this.ackTimeoutMs, Math.max(1, deadlineAt - Date.now())),
        )
      } catch {
        entry.failover(generation, socket)
      }
    }

    entry.id = id
    entry.resource = resource
    entry.candidates = candidates
    entry.epochs = epochs
    entry.attempt = 0
    entry.generation = 0
    entry.socket = candidates[0]
    entry.acked = false
    entry.ackTimer = null
    entry.deadlineAt = deadlineAt
    entry.deadlineTimer = setTimeout(() => deliver(null), this.deadlines[resource])
    entry.deliver = deliver
    entry.onAck = (generation, socket) => {
      if (delivered || generation !== entry.generation || socket !== entry.socket) return
      entry.acked = true
      if (entry.ackTimer) clearTimeout(entry.ackTimer)
      entry.ackTimer = null
    }
    entry.failover = (generation, socket) => {
      if (delivered || generation !== entry.generation || socket !== entry.socket) return
      entry.generation += 1
      // Reset per-attempt ack state synchronously so a deadline firing while the
      // deprioritization write is pending cannot read the abandoned candidate's
      // ack against the next candidate's epoch (mis-deprioritizing a healthy
      // socket that never received the request).
      entry.acked = false
      if (entry.ackTimer) clearTimeout(entry.ackTimer)
      entry.ackTimer = null
      const failedEpoch = entry.epochs[entry.attempt]
      const canAdvance = entry.attempt + 1 < Math.min(entry.candidates.length, MAX_FAILOVER)
        && Date.now() < entry.deadlineAt
      if (canAdvance) {
        entry.attempt += 1
        entry.socket = entry.candidates[entry.attempt]
      }
      void this.addDeprioritized(resource, failedEpoch).then(() => {
        if (delivered) return
        if (canAdvance) sendToCandidate()
        else deliver(null)
      }, () => deliver(null))
    }
    entry.promise = (async () => {
      try {
        await this.relayState.storage.put(`nextAllowedAt:${resource}`, Date.now() + this.minRefreshMs)
        if (delivered) return { kind: 'fallback' }
        // A close/error may have synchronously claimed failover while the
        // cooldown write was pending. Only the untouched initial generation
        // owns the first send; the failover path sends or settles otherwise.
        if (entry.generation === 0) sendToCandidate()
        const frame = await deferred
        const epoch = entry.epochs[entry.attempt]
        if (frame === null) {
          if (entry.acked) await this.addDeprioritized(resource, epoch)
          return { kind: 'fallback' }
        }
        const hasPayload = Object.prototype.hasOwnProperty.call(frame, 'payload')
        const hasError = Object.prototype.hasOwnProperty.call(frame, 'error')
        if (hasPayload === hasError || !hasPayload || !this.isPayload(frame.payload)) {
          await this.addDeprioritized(resource, epoch)
          return { kind: 'fallback' }
        }
        if (resource === 'dashboard') {
          await this.storeDashboard(frame.payload)
          await this.clearDeprioritized(resource, epoch)
          return { kind: 'fresh', body: { ...frame.payload, stale: false } }
        }
        await this.clearDeprioritized(resource, epoch)
        return { kind: 'fresh', body: frame.payload }
      } catch (error) {
        console.error('relay request finalization failed', error)
        return { kind: 'fallback' }
      } finally {
        clearTimeout(entry.deadlineTimer)
        if (entry.ackTimer) clearTimeout(entry.ackTimer)
        this.inflight.delete(resource)
      }
    })()
    this.inflight.set(resource, entry)
    return entry.promise
  }

  private failInflightForSocket(ws: WebSocket): void {
    for (const entry of this.inflight.values()) {
      if (entry.socket === ws) entry.failover(entry.generation, ws)
    }
  }

  private async selectCandidates(resource: Resource): Promise<{ sockets: WebSocket[]; epochs: number[] }> {
    const open = this.relayState.getWebSockets()
      .map((socket) => ({ socket, attachment: this.attachment(socket) }))
      .filter(({ socket, attachment }) => socket.readyState === WebSocket.OPEN && attachment.caps.includes(resource))
    const liveEpochs = new Set(open.map(({ attachment }) => attachment.epoch))
    const deprioritized = await this.getDeprioritized(resource)
    const pruned = new Set([...deprioritized].filter((epoch) => liveEpochs.has(epoch)))
    if (pruned.size !== deprioritized.size) await this.putDeprioritized(resource, pruned)
    open.sort((left, right) => {
      const leftBad = pruned.has(left.attachment.epoch) ? 1 : 0
      const rightBad = pruned.has(right.attachment.epoch) ? 1 : 0
      return leftBad - rightBad || right.attachment.epoch - left.attachment.epoch
    })
    return {
      sockets: open.map(({ socket }) => socket),
      epochs: open.map(({ attachment }) => attachment.epoch),
    }
  }

  private attachment(ws: WebSocket): SocketAttachment {
    const raw = ws.deserializeAttachment() as Partial<SocketAttachment> | null
    return {
      epoch: typeof raw?.epoch === 'number' ? raw.epoch : 0,
      caps: Array.isArray(raw?.caps)
        ? raw.caps.filter((cap): cap is Resource => cap === 'dashboard' || cap === 'manifest')
        : [],
    }
  }

  private parseCapabilities(value: unknown): Resource[] | null {
    if (!Array.isArray(value) || value.length > MAX_CAPABILITIES) return null
    if (!value.every((cap) => typeof cap === 'string')) return null
    return [...new Set(value.filter((cap): cap is Resource => cap === 'dashboard' || cap === 'manifest'))]
  }

  private isPayload(value: unknown): value is DashboardPayload {
    return !!value && typeof value === 'object' && !Array.isArray(value)
  }

  private async snapshotResult(request: Request, forceStale: boolean): Promise<Settled> {
    const stored = await this.relayState.storage.get<StoredDashboard>('dashboard')
    if (!stored) return { kind: 'fallback' }
    const staleAfterMs = Number(request.headers.get('x-relay-stale-after-ms') ?? STALE_AFTER_MS)
    const timestamp = Date.parse(stored.lastPublishAt)
    const stale = forceStale || !Number.isFinite(timestamp) || Date.now() - timestamp > staleAfterMs
    return { kind: 'snapshot', body: { ...stored.payload, stale } }
  }

  private async responseFor(resource: Resource, result: Settled, request: Request): Promise<Response> {
    if (result.kind === 'fresh' || result.kind === 'snapshot') return deviceResponse(result.body)
    if (resource === 'manifest') {
      return deviceResponse({ error: 'Manifest unavailable' }, { status: 404 })
    }
    const snapshot = await this.snapshotResult(request, true)
    return snapshot.kind === 'snapshot'
      ? deviceResponse(snapshot.body)
      : deviceResponse({ error: 'Dashboard not published yet' }, { status: 404 })
  }

  private async authorizeDevice(request: Request): Promise<boolean> {
    return authorized(request, request.headers.get('x-relay-device-token') ?? undefined)
  }

  private async storeDashboard(payload: DashboardPayload): Promise<void> {
    const existing = await this.relayState.storage.get<StoredDashboard>('dashboard')
    const updatedAt = typeof payload.updatedAt === 'string' ? payload.updatedAt : null
    if (existing && existing.updatedAt === updatedAt) return
    const now = new Date().toISOString()
    await this.relayState.storage.put('dashboard', { payload, lastPublishAt: now, updatedAt } satisfies StoredDashboard)
    await this.relayState.storage.put('lastPublishAt', now)
    await this.increment('publishCount')
  }

  private async getDeprioritized(resource: Resource): Promise<Set<number>> {
    return new Set(await this.relayState.storage.get<number[]>(`deprioritized:${resource}`) ?? [])
  }

  private async putDeprioritized(resource: Resource, epochs: Set<number>): Promise<void> {
    await this.relayState.storage.put(`deprioritized:${resource}`, [...epochs])
  }

  private async addDeprioritized(resource: Resource, epoch: number): Promise<void> {
    const epochs = await this.getDeprioritized(resource)
    epochs.add(epoch)
    await this.putDeprioritized(resource, epochs)
  }

  private async clearDeprioritized(resource: Resource, epoch: number): Promise<void> {
    const epochs = await this.getDeprioritized(resource)
    if (epochs.delete(epoch)) await this.putDeprioritized(resource, epochs)
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
    if (uuid && UUID_RE.test(uuid)) await this.relayState.storage.put('uuid', uuid)
  }

  private async stats(): Promise<DeviceStats> {
    return {
      uuid: await this.relayState.storage.get<string>('uuid') ?? this.relayState.id.toString(),
      connected: this.relayState.getWebSockets().some((socket) => socket.readyState === WebSocket.OPEN),
      lastPublishAt: await this.relayState.storage.get<string>('lastPublishAt') ?? null,
      publishCount: await this.relayState.storage.get<number>('publishCount') ?? 0,
      lastFetchAt: await this.relayState.storage.get<string>('lastFetchAt') ?? null,
      fetchCount: await this.relayState.storage.get<number>('fetchCount') ?? 0,
    }
  }
}
