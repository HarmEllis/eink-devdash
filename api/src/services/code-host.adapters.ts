import { resolveCodeHostProvider, type CodeHostProvider } from './code-host.config.js'
import type { DashboardServiceAdapter } from './dashboard-service.js'
import { createGitHubServiceAdapter } from './github.service.js'

export type GitLabCodeHostConfig = {
  provider: 'gitlab'
  token?: string
  baseUrl?: string
}

export type GitHubCodeHostConfig = {
  provider: 'github'
  token?: string
  notificationsToken?: string
}

export type DisabledCodeHostConfig = {
  provider: 'none'
}

export type CodeHostConfig =
  | GitHubCodeHostConfig
  | GitLabCodeHostConfig
  | DisabledCodeHostConfig

export function codeHostConfigFromEnv(
  env: NodeJS.ProcessEnv = process.env,
  provider: CodeHostProvider = resolveCodeHostProvider(env),
): CodeHostConfig {
  if (provider === 'github') {
    return {
      provider,
      token: env.GITHUB_TOKEN,
      notificationsToken: env.GITHUB_NOTIFICATIONS_TOKEN,
    }
  }
  if (provider === 'gitlab') {
    return {
      provider,
      token: env.GITLAB_TOKEN,
      baseUrl: env.GITLAB_BASE_URL,
    }
  }
  return { provider: 'none' }
}

export function createCodeHostAdapters(
  config: CodeHostConfig = codeHostConfigFromEnv(),
): DashboardServiceAdapter[] {
  if (config.provider === 'github') {
    return [createGitHubServiceAdapter({
      token: config.token,
      notificationsToken: config.notificationsToken,
    })]
  }
  return []
}
