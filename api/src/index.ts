import Fastify from 'fastify'
import { dashboardRoute } from './routes/dashboard.js'

const app = Fastify({ logger: true })

const DEVICE_TOKEN = process.env.DEVICE_TOKEN
if (!DEVICE_TOKEN) throw new Error('DEVICE_TOKEN env var required')

app.addHook('onRequest', async (req, reply) => {
  if (req.url === '/health') return
  const auth = req.headers.authorization
  if (!auth || auth !== `Bearer ${DEVICE_TOKEN}`) {
    reply.code(401).send({ error: 'Unauthorized' })
  }
})

app.get('/health', async () => ({ ok: true }))
app.register(dashboardRoute)

app.listen({ port: 3000, host: '0.0.0.0' }, (err) => {
  if (err) { app.log.error(err); process.exit(1) }
})
