import type { DashboardPayload } from '../routes/dashboard.js'
import type { OtaManifest } from '../routes/ota.js'
import WebSocket from 'ws'

export type RelayLogger = {
  info(obj: unknown, msg?: string): void
  warn(obj: unknown, msg?: string): void
  error(obj: unknown, msg?: string): void
  debug?(obj: unknown, msg?: string): void
}

export type RelayConfig = {
  enabled: boolean
  relayUrl: string
  publishKey: string
  deviceUuid: string
  publishIntervalMs: number
  reconnectMinMs: number
  reconnectMaxMs: number
}

export type RelayPublisherOptions = {
  config?: RelayConfig
  env?: NodeJS.ProcessEnv
  logger: RelayLogger
  getPayload: (signal?: AbortSignal) => Promise<DashboardPayload>
  getManifest?: () => Promise<OtaManifest> | OtaManifest
}

export type RelayPublisher = {
  start(): void
  stop(): void
}

const DEFAULT_PUBLISH_INTERVAL_MS = 300_000
const DEFAULT_RECONNECT_MIN_MS = 1_000
const DEFAULT_RECONNECT_MAX_MS = 60_000

function optionalEnv(value: string | undefined): string {
  return value?.trim() ?? ''
}

function intEnv(value: string | undefined, fallback: number): number {
  if (!value) return fallback
  const parsed = Number.parseInt(value, 10)
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback
}

export function relayConfigFromEnv(env: NodeJS.ProcessEnv = process.env): RelayConfig {
  return {
    enabled: env.RELAY_ENABLED === 'true',
    relayUrl: optionalEnv(env.RELAY_URL),
    publishKey: optionalEnv(env.RELAY_PUBLISH_KEY),
    deviceUuid: optionalEnv(env.DEVICE_UUID),
    publishIntervalMs: intEnv(env.RELAY_PUBLISH_INTERVAL_MS, DEFAULT_PUBLISH_INTERVAL_MS),
    reconnectMinMs: intEnv(env.RELAY_RECONNECT_MIN_MS, DEFAULT_RECONNECT_MIN_MS),
    reconnectMaxMs: intEnv(env.RELAY_RECONNECT_MAX_MS, DEFAULT_RECONNECT_MAX_MS),
  }
}

export function validateRelayConfig(config: RelayConfig): string[] {
  if (!config.enabled) return []
  const missing: string[] = []
  if (!config.relayUrl) missing.push('RELAY_URL')
  if (!config.publishKey) missing.push('RELAY_PUBLISH_KEY')
  if (!config.deviceUuid) missing.push('DEVICE_UUID')
  return missing
}

export function buildRelayConnectUrl(relayUrl: string, deviceUuid: string): string {
  const url = new URL(relayUrl)
  if (url.protocol === 'https:') url.protocol = 'wss:'
  else if (url.protocol === 'http:') url.protocol = 'ws:'
  else if (url.protocol !== 'ws:' && url.protocol !== 'wss:') {
    throw new Error(`Unsupported RELAY_URL protocol: ${url.protocol}`)
  }
  url.pathname = '/connect'
  url.searchParams.set('uuid', deviceUuid)
  return url.toString()
}

export function createRelayPublisher(options: RelayPublisherOptions): RelayPublisher {
  const config = options.config ?? relayConfigFromEnv(options.env)
  const missing = validateRelayConfig(config)
  const logger = options.logger

  let ws: WebSocket | null = null
  const publishTimers = new Set<ReturnType<typeof setInterval>>()
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let reconnectDelayMs = config.reconnectMinMs
  let stopped = false

  function clearTimers() {
    for (const timer of publishTimers) clearInterval(timer)
    publishTimers.clear()
    if (reconnectTimer) clearTimeout(reconnectTimer)
    reconnectTimer = null
  }

  function scheduleReconnect() {
    if (stopped || reconnectTimer) return
    const delay = reconnectDelayMs
    reconnectDelayMs = Math.min(reconnectDelayMs * 2, config.reconnectMaxMs)
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null
      connect()
    }, delay)
    logger.warn({ delayMs: delay }, 'relay websocket disconnected; reconnect scheduled')
  }

  async function publishCurrentDashboard(socket: WebSocket) {
    if (socket.readyState !== WebSocket.OPEN) return
    try {
      const payload = await options.getPayload()
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ type: 'dashboard', payload }))
      }
    } catch (err) {
      logger.warn({ err }, 'relay dashboard publish failed')
    }
  }

  async function handleRequest(
    socket: WebSocket,
    frame: { id: string; resource: 'dashboard' | 'manifest' },
  ) {
    if (socket.readyState !== WebSocket.OPEN) return
    socket.send(JSON.stringify({ type: 'request-ack', id: frame.id }))
    try {
      const payload = frame.resource === 'dashboard'
        ? await options.getPayload()
        : options.getManifest
          ? await options.getManifest()
          : undefined
      if (payload === undefined) throw new Error(`Unsupported relay resource: ${frame.resource}`)
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
          type: 'response',
          id: frame.id,
          resource: frame.resource,
          payload,
        }))
      }
    } catch (err) {
      logger.warn({ err, resource: frame.resource }, 'relay on-demand request failed')
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
          type: 'response',
          id: frame.id,
          resource: frame.resource,
          error: 'request_failed',
        }))
      }
    }
  }

  function connect() {
    if (stopped || !config.enabled) return
    let connectUrl: string
    try {
      connectUrl = buildRelayConnectUrl(config.relayUrl, config.deviceUuid)
    } catch (err) {
      logger.error({ err }, 'relay websocket URL is invalid')
      return
    }

    const socket = new WebSocket(connectUrl, {
      headers: { authorization: `Bearer ${config.publishKey}` },
    })
    ws = socket

    socket.on('open', () => {
      reconnectDelayMs = config.reconnectMinMs
      logger.info({ relayUrl: config.relayUrl, deviceUuid: config.deviceUuid }, 'relay websocket connected')
      const capabilities = ['dashboard', ...(options.getManifest ? ['manifest'] : [])]
      socket.send(JSON.stringify({ type: 'hello', capabilities }))
      void publishCurrentDashboard(socket)
      const timer = setInterval(() => void publishCurrentDashboard(socket), config.publishIntervalMs)
      publishTimers.add(timer)
      socket.once('close', () => {
        clearInterval(timer)
        publishTimers.delete(timer)
      })
    })
    socket.on('message', (data) => {
      const text = data.toString()
      let frame: unknown
      try {
        frame = JSON.parse(text)
      } catch {
        logger.warn('relay websocket received invalid JSON')
        return
      }
      if (
        frame
        && typeof frame === 'object'
        && (frame as { type?: unknown }).type === 'request'
        && typeof (frame as { id?: unknown }).id === 'string'
        && ((frame as { resource?: unknown }).resource === 'dashboard'
          || (frame as { resource?: unknown }).resource === 'manifest')
      ) {
        void handleRequest(socket, frame as { id: string; resource: 'dashboard' | 'manifest' })
        return
      }
      logger.debug?.({ message: text }, 'relay websocket message')
    })
    socket.on('close', () => {
      if (ws === socket) ws = null
      scheduleReconnect()
    })
    socket.on('error', (err) => {
      logger.warn({ err }, 'relay websocket error')
    })
  }

  return {
    start() {
      if (!config.enabled) {
        logger.info('relay publisher disabled')
        return
      }
      if (missing.length > 0) {
        logger.warn({ missing }, 'relay publisher enabled but not configured')
        return
      }
      stopped = false
      connect()
    },

    stop() {
      stopped = true
      clearTimers()
      if (ws && ws.readyState < WebSocket.CLOSING) {
        ws.close()
      }
      ws = null
    },
  }
}
