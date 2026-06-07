import type { FastifyInstance } from 'fastify'

/*
 * Owner/repo slug for the firmware GitHub Releases. Hard-coded on purpose
 * (not read from env) so a wrongly-deployed fork can't silently advertise
 * a download URL that points at someone else's binaries. Bumping this is
 * a code change that goes through review.
 */
const FIRMWARE_REPO_SLUG = 'HarmEllis/eink-devdash'

const FIRMWARE_ASSET_NAME = 'eink-devdash.bin'

export function buildDownloadUrl(version: string): string {
  return `https://github.com/${FIRMWARE_REPO_SLUG}/releases/download/${version}/${FIRMWARE_ASSET_NAME}`
}

export function getRepoSlug(): string {
  return FIRMWARE_REPO_SLUG
}

interface OtaManifestDisabled {
  otaEnabled: false
}

interface OtaManifestEnabled {
  otaEnabled: true
  latestVersion: string
  downloadUrl: string
}

export type OtaManifest = OtaManifestDisabled | OtaManifestEnabled

const UINT32_MAX = 0xffff_ffff
const CANONICAL_VERSION_RE = /^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/

export function appVersionIsCanonical(version: string): boolean {
  const match = CANONICAL_VERSION_RE.exec(version)
  return !!match && match.slice(1).every((part) => {
    const value = Number(part)
    return Number.isSafeInteger(value) && value <= UINT32_MAX
  })
}

export function buildManifest(opts: {
  otaEnabled: boolean
  appVersion: string
}): OtaManifest {
  if (!opts.otaEnabled) return { otaEnabled: false }
  if (!appVersionIsCanonical(opts.appVersion)) return { otaEnabled: false }
  return {
    otaEnabled: true,
    latestVersion: opts.appVersion,
    downloadUrl: buildDownloadUrl(opts.appVersion),
  }
}

function readOtaEnv() {
  return {
    otaEnabled: process.env.OTA_ENABLED !== 'false',
    appVersion: process.env.APP_VERSION ?? '',
  }
}

export function getOtaManifest(): OtaManifest {
  const env = readOtaEnv()
  const manifest = buildManifest(env)
  if (env.otaEnabled && !appVersionIsCanonical(env.appVersion)) {
    console.warn(`[ota] APP_VERSION is not canonical; OTA disabled: ${env.appVersion || '(unset)'}`)
  }
  return manifest
}

export async function otaRoute(app: FastifyInstance) {
  const initial = readOtaEnv()
  if (initial.otaEnabled && !initial.appVersion) {
    app.log.warn(
      'OTA_ENABLED=true but APP_VERSION is empty; /ota/manifest will report otaEnabled=false',
    )
  } else {
    app.log.info(
      {
        otaEnabled: initial.otaEnabled,
        appVersion: initial.appVersion || '(unset)',
      },
      'OTA manifest route registered',
    )
  }

  app.get('/ota/manifest', async (_req, reply) => {
    return reply.send(getOtaManifest())
  })
}
