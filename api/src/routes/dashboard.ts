import type { FastifyInstance } from 'fastify'
import { getGitHubStats } from '../services/github.service.js'
import { getClaudeUsage } from '../services/claude.service.js'
import { getCodexUsage } from '../services/codex.service.js'

const DASHBOARD_TIME_ZONE = process.env.DASHBOARD_TIME_ZONE
  ?? process.env.TZ
  ?? 'Europe/Amsterdam'

function formatLocalUpdatedAt(now: Date): string {
  return new Intl.DateTimeFormat('en-GB', {
    timeZone: DASHBOARD_TIME_ZONE,
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).format(now)
}

export async function dashboardRoute(app: FastifyInstance) {
  app.get('/dashboard', async (_req, reply) => {
    const now = new Date()
    const [github, claude, codex] = await Promise.all([
      getGitHubStats(),
      getClaudeUsage(),
      getCodexUsage(),
    ])

    const body = {
      schemaVersion: 1,
      ...(github ? { github } : {}),
      claude,
      codex,
      updatedAt: now.toISOString(),
      updatedAtLocal: formatLocalUpdatedAt(now),
    }

    return reply.send(body)
  })
}
