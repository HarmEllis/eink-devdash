import { Bonjour } from 'bonjour-service'
import { createApp } from './app.js'
import { buildDashboardPayload, createDashboardAdapters } from './routes/dashboard.js'
import { createRelayPublisher, type RelayPublisher } from './relay/relay-client.js'

const PORT = 3000

const DEVICE_TOKEN = process.env.DEVICE_TOKEN
if (!DEVICE_TOKEN) throw new Error('DEVICE_TOKEN env var required')
const MDNS_ENABLED = process.env.MDNS_ENABLED !== 'false'
const MDNS_NAME = process.env.MDNS_NAME || 'devdash-api'

const app = createApp({ deviceToken: DEVICE_TOKEN })

let bonjour: Bonjour | null = null
let mdnsService: ReturnType<Bonjour['publish']> | null = null
let relayPublisher: RelayPublisher | null = null

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
