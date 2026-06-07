import Fastify, { type FastifyInstance, type FastifyServerOptions } from 'fastify'
import { createDashboardCoordinator, type DashboardCoordinator } from './dashboard-coordinator.js'
import { createDashboardAdapters, dashboardRoute } from './routes/dashboard.js'
import { otaRoute } from './routes/ota.js'
import type { DashboardServiceAdapter } from './services/dashboard-service.js'

export type CreateAppOptions = {
  // Bearer token required on every route except /health. The direct LAN route
  // and the relay device fetch share this same token.
  deviceToken: string
  // Inject dashboard adapters in tests; production uses the real set.
  adapters?: DashboardServiceAdapter[]
  coordinator?: DashboardCoordinator
  logger?: FastifyServerOptions['logger']
}

/**
 * Build the Fastify app with auth + routes but no side effects (no listen, no
 * mDNS, no relay publisher). This keeps the direct ESP -> API route fully
 * testable and independent of relay state: the auth hook and /dashboard work
 * regardless of whether the relay is enabled or configured.
 */
export function createApp(options: CreateAppOptions): FastifyInstance {
  const app = Fastify({ logger: options.logger ?? true })
  const coordinator = options.coordinator
    ?? createDashboardCoordinator(
      options.adapters ?? createDashboardAdapters(),
      process.env.DASHBOARD_TIME_ZONE,
    )

  app.addHook('onRequest', async (req, reply) => {
    if (req.url === '/health') return
    const auth = req.headers.authorization
    if (!auth || auth !== `Bearer ${options.deviceToken}`) {
      return reply.code(401).send({ error: 'Unauthorized' })
    }
  })

  app.get('/health', async () => ({ ok: true }))
  app.register(dashboardRoute, { coordinator })
  app.register(otaRoute)

  return app
}
