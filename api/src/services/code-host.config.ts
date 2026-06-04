export type CodeHostProvider = 'github' | 'gitlab' | 'none'
export type CodeHostProviderEnv = {
  CODE_HOST_PROVIDER?: string
  GITHUB_TOKEN?: string
}

export function resolveCodeHostProvider(
  env: CodeHostProviderEnv = process.env,
): CodeHostProvider {
  const configured = env.CODE_HOST_PROVIDER?.trim().toLowerCase()

  if (!configured) {
    return env.GITHUB_TOKEN?.trim() ? 'github' : 'none'
  }

  if (configured === 'github' || configured === 'gitlab' || configured === 'none') {
    return configured
  }

  console.warn(`[code-host] unsupported CODE_HOST_PROVIDER="${configured}"; disabling code-host service`)
  return 'none'
}
