import type { FastifyInstance } from 'fastify'
import { getGitHubStats } from '../services/github.service.js'
import { getClaudeUsage } from '../services/claude.service.js'
import { getCodexUsage } from '../services/codex.service.js'

export async function dashboardRoute(app: FastifyInstance) {
  app.get('/api/dashboard', async (_req, reply) => {
    const [github, claude, codex] = await Promise.all([
      getGitHubStats(),
      getClaudeUsage(),
      getCodexUsage(),
    ])

    return reply.send({
      schemaVersion: 1,
      github,
      claude,
      codex,
      updatedAt: new Date().toISOString(),
    })
  })
}
