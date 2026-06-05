import Fastify from 'fastify'
import { Bonjour } from 'bonjour-service'
import { buildDashboardPayload, createDashboardAdapters, dashboardRoute } from './routes/dashboard.js'
import { otaRoute } from './routes/ota.js'
import { createRelayPublisher, type RelayPublisher } from './relay/relay-client.js'

const app = Fastify({ logger: true })
const PORT = 3000

const DEVICE_TOKEN = process.env.DEVICE_TOKEN
if (!DEVICE_TOKEN) throw new Error('DEVICE_TOKEN env var required')
const MDNS_ENABLED = process.env.MDNS_ENABLED !== 'false'
const MDNS_NAME = process.env.MDNS_NAME || 'devdash-api'

let bonjour: Bonjour | null = null
let mdnsService: ReturnType<Bonjour['publish']> | null = null
let relayPublisher: RelayPublisher | null = null

app.addHook('onRequest', async (req, reply) => {
  if (req.url === '/health') return
  const auth = req.headers.authorization
  if (!auth || auth !== `Bearer ${DEVICE_TOKEN}`) {
    return reply.code(401).send({ error: 'Unauthorized' })
  }
})

app.get('/health', async () => ({ ok: true }))
app.register(dashboardRoute)
app.register(otaRoute)

function startMdns() {
  if (!MDNS_ENABLED) {
    app.log.info('mDNS advertisement disabled')
    return
  }
  bonjour = new Bonjour()
  mdnsService = bonjour.publish({ name: MDNS_NAME, type: 'http', port: PORT })
  app.log.info({ name: MDNS_NAME, port: PORT }, 'mDNS advertisement started')
}

async function shutdown(signal: string) {
  app.log.info({ signal }, 'shutting down')
  relayPublisher?.stop()
  bonjour?.destroy()
  await app.close()
}

for (const signal of ['SIGINT', 'SIGTERM'] as const) {
  process.once(signal, () => {
    shutdown(signal)
      .then(() => process.exit(0))
      .catch((err) => {
        app.log.error(err)
        process.exit(1)
      })
  })
}

app.listen({ port: PORT, host: '0.0.0.0' }, (err) => {
  if (err) { app.log.error(err); process.exit(1) }
  startMdns()
  relayPublisher = createRelayPublisher({
    logger: app.log,
    getPayload: () => buildDashboardPayload(new Date(), createDashboardAdapters()),
  })
  relayPublisher.start()
})
