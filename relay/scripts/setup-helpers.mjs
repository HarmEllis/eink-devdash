// Pure, dependency-free helpers for the relay self-host setup script.
//
// These functions use only web-standard globals (globalThis.crypto, btoa,
// URL, string ops) and never touch the filesystem, network, or child
// processes. That keeps them trivially unit-testable and lets setup.mjs stay a
// thin orchestrator around them.

const DEFAULT_TOKEN_BYTES = 32

/** Encode bytes as URL-safe base64 (base64url, no padding). */
export function base64url(bytes) {
  let binary = ''
  for (const byte of bytes) binary += String.fromCharCode(byte)
  return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
}

/** Cryptographically-random URL-safe secret. */
export function generateToken(bytes = DEFAULT_TOKEN_BYTES) {
  const buffer = new Uint8Array(bytes)
  crypto.getRandomValues(buffer)
  return base64url(buffer)
}

/**
 * A fresh self-host identity: one UUID v4 plus three independent secrets.
 * - deviceToken authenticates both the direct API route and the relay device
 *   fetch (same value, see api/src/index.ts onRequest hook + relay
 *   parseDeviceTokens single-pair fallback).
 * - relayPublishKey authorizes the publisher WebSocket.
 * - adminKey authorizes /admin/stats on the Worker.
 */
export function generateIdentity() {
  return {
    deviceUuid: crypto.randomUUID(),
    deviceToken: generateToken(),
    relayPublishKey: generateToken(),
    adminKey: generateToken(),
  }
}

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i
const ENV_KEY_RE = /^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=/

function envKeyOf(line) {
  const match = ENV_KEY_RE.exec(line)
  return match ? match[1] : null
}

/**
 * Merge KEY=VALUE updates into existing .env text without clobbering unrelated
 * lines. Keys already present are updated in place (preserving surrounding
 * order and comments); new keys are appended. Returns the new file content with
 * exactly one trailing newline.
 */
export function mergeEnv(existing, updates) {
  const seen = new Set()
  const rawLines = existing.length === 0 ? [] : existing.split('\n')

  const merged = rawLines.map((line) => {
    const key = envKeyOf(line)
    if (key && Object.prototype.hasOwnProperty.call(updates, key)) {
      seen.add(key)
      return `${key}=${updates[key]}`
    }
    return line
  })

  // Drop a single trailing blank line (from a previous trailing newline) so
  // appended keys do not leave a gap; we re-add the newline at the end.
  while (merged.length > 0 && merged[merged.length - 1].trim() === '') {
    merged.pop()
  }

  for (const [key, value] of Object.entries(updates)) {
    if (!seen.has(key)) merged.push(`${key}=${value}`)
  }

  return merged.join('\n') + '\n'
}

export function parseEnvValues(existing) {
  const values = {}
  for (const line of existing.split('\n')) {
    const key = envKeyOf(line)
    if (!key) continue
    const separator = line.indexOf('=')
    values[key] = line.slice(separator + 1).trim()
  }
  return values
}

/**
 * Reuse a complete local identity so setup is idempotent on one machine.
 * Legacy setup files have no RELAY_ADMIN_KEY; add only that missing secret.
 */
export function identityFromEnv(existing) {
  const env = parseEnvValues(existing)
  if (!env.DEVICE_UUID && !env.RELAY_PUBLISH_KEY) return null
  const present = ['DEVICE_UUID', 'DEVICE_TOKEN', 'RELAY_PUBLISH_KEY']
    .filter((key) => Boolean(env[key]))
  if (present.length !== 3) {
    throw new Error(
      'Existing .env has an incomplete relay identity; expected DEVICE_UUID, '
      + 'DEVICE_TOKEN, and RELAY_PUBLISH_KEY together.',
    )
  }
  if (!UUID_RE.test(env.DEVICE_UUID)) {
    throw new Error('Existing DEVICE_UUID is not a valid UUID.')
  }
  return {
    deviceUuid: env.DEVICE_UUID,
    deviceToken: env.DEVICE_TOKEN,
    relayPublishKey: env.RELAY_PUBLISH_KEY,
    adminKey: env.RELAY_ADMIN_KEY || generateToken(),
  }
}

/**
 * Classify the relay identity an existing .env holds, without throwing — the
 * decision-layer counterpart to identityFromEnv (which extracts/validates):
 * - 'none'      no relay identity (a lone DEVICE_TOKEN is a direct-API token).
 * - 'complete'  DEVICE_UUID (valid) + DEVICE_TOKEN + RELAY_PUBLISH_KEY all present.
 * - 'partial'   some-but-not-all of those, or an invalid DEVICE_UUID.
 */
export function classifyEnvIdentity(existing) {
  const env = parseEnvValues(existing)
  if (!env.DEVICE_UUID && !env.RELAY_PUBLISH_KEY) return 'none'
  const complete = Boolean(env.DEVICE_UUID) && Boolean(env.DEVICE_TOKEN)
    && Boolean(env.RELAY_PUBLISH_KEY) && UUID_RE.test(env.DEVICE_UUID)
  return complete ? 'complete' : 'partial'
}

/** Normalize RELAY_SETUP_IDENTITY: 'reuse' | 'new' | undefined; else throws. */
export function normalizeIdentityOverride(raw) {
  if (raw == null || raw === '') return undefined
  const value = String(raw).trim().toLowerCase()
  if (value === 'reuse' || value === 'new') return value
  throw new Error(`RELAY_SETUP_IDENTITY must be "reuse" or "new" (got "${raw}").`)
}

/**
 * Decide how setup handles this machine's device identity. `existing` is the
 * classifyEnvIdentity result ('none' | 'complete' | 'partial'). Returns one of
 * 'reuse' | 'new' | 'prompt-reuse' | 'prompt-new'. Throws when an explicit
 * RELAY_SETUP_IDENTITY=reuse has nothing complete to reuse, or when a partial
 * identity is hit non-interactively with no override — both must fail loudly
 * rather than silently minting a new identity.
 */
export function chooseIdentityMode({ existing, override, isInteractive }) {
  if (override === 'reuse') {
    if (existing !== 'complete') {
      throw new Error(
        `RELAY_SETUP_IDENTITY=reuse, but the target .env has no complete identity `
        + `(found: ${existing}). Check the file / mount, or unset the override.`,
      )
    }
    return 'reuse'
  }
  if (override === 'new') return 'new'
  if (existing === 'none') return 'new'
  if (existing === 'complete') return isInteractive ? 'prompt-reuse' : 'reuse'
  // existing === 'partial'
  if (isInteractive) return 'prompt-new'
  throw new Error(
    'The target .env has an incomplete relay identity (need DEVICE_UUID, '
    + 'DEVICE_TOKEN, and RELAY_PUBLISH_KEY together). Fix or clear it, set '
    + 'RELAY_SETUP_IDENTITY=new to replace it, or run with a TTY to choose.',
  )
}

/** Parse a reuse/new prompt answer. Bare Enter => 'reuse'. null => re-ask. */
export function normalizeReuseAnswer(raw) {
  const value = String(raw ?? '').trim().toLowerCase()
  if (value === '' || value === 'r' || value === 'reuse' || value === 'y' || value === 'yes') {
    return 'reuse'
  }
  if (value === 'n' || value === 'new' || value === 'no') return 'new'
  return null
}

/** Parse a yes/no prompt answer. Bare Enter => 'yes'. null => re-ask. */
export function normalizeYesNo(raw) {
  const value = String(raw ?? '').trim().toLowerCase()
  if (value === '' || value === 'y' || value === 'yes') return 'yes'
  if (value === 'n' || value === 'no') return 'no'
  return null
}

export function workerSecretName(deviceUuid) {
  if (!UUID_RE.test(deviceUuid)) throw new Error('Cannot build secret names for an invalid UUID.')
  const suffix = deviceUuid.replace(/-/g, '').toUpperCase()
  return `DEVICE_IDENTITY_${suffix}`
}

export function workerIdentitySecret(identity) {
  return JSON.stringify({
    deviceToken: identity.deviceToken,
    publishKey: identity.relayPublishKey,
    adminKey: identity.adminKey,
  })
}

/** The values setup writes/merges into the root .env for Docker + the API. */
export function relayEnvUpdates({ deviceUuid, deviceToken, relayPublishKey, adminKey, workerUrl }) {
  return {
    DEVICE_TOKEN: deviceToken,
    DEVICE_UUID: deviceUuid,
    RELAY_ENABLED: 'true',
    RELAY_URL: workerUrl,
    RELAY_PUBLISH_KEY: relayPublishKey,
    RELAY_ADMIN_KEY: adminKey,
  }
}

/** The relay dashboard URL the firmware fetches: https://<worker>/d/<uuid>. */
export function buildRelayDashboardUrl(workerUrl, deviceUuid) {
  const url = new URL(workerUrl)
  url.pathname = `/d/${deviceUuid}`
  url.search = ''
  url.hash = ''
  return url.toString()
}

/**
 * Extract the deployed Worker URL from `wrangler deploy` stdout. Wrangler prints
 * a line like "https://eink-devdash-relay.<subdomain>.workers.dev". Returns the
 * first match, or null when no workers.dev URL is present (custom domain / route
 * deployments, where the caller must supply the URL another way).
 */
export function parseWorkerUrl(deployStdout) {
  const match = /https:\/\/[a-z0-9-]+\.[a-z0-9-]+\.workers\.dev/i.exec(deployStdout ?? '')
  return match ? match[0] : null
}

/**
 * Human-readable provisioning block printed at the end of setup. Kept pure (no
 * I/O) so it can be asserted in tests; setup.mjs prints this and renders the QR
 * for relayDashboardUrl separately.
 */
export function formatProvisioningSummary({ relayDashboardUrl, deviceToken, adminKey, workerUrl }) {
  return [
    'Relay deployed. Provision your firmware with these values:',
    '',
    `  API URL (captive portal):  ${relayDashboardUrl}`,
    `  Device token:              ${deviceToken}`,
    '',
    'Keep these secret. Admin stats:',
    '',
    `  curl -H "Authorization: Bearer ${adminKey}" ${workerUrl}/admin/stats`,
  ].join('\n')
}

/**
 * A ready-to-paste .env block (all relay keys). Used for the opt-in
 * RELAY_SETUP_PRINT_ENV path when the written file is not convenient (e.g. a
 * container run without a mounted output volume).
 */
export function formatEnvBlock(identity, workerUrl) {
  const updates = relayEnvUpdates({ ...identity, workerUrl })
  return Object.entries(updates).map(([key, value]) => `${key}=${value}`).join('\n')
}

/**
 * Decide how to authenticate wrangler:
 * - 'token'       CLOUDFLARE_API_TOKEN is set — non-interactive, no browser.
 * - 'login'       no token but a TTY is available — interactive OAuth.
 * - 'unavailable' neither — the caller must fail with guidance.
 */
export function chooseAuthMode({ hasToken, isInteractive }) {
  if (hasToken) return 'token'
  if (isInteractive) return 'login'
  return 'unavailable'
}

/**
 * `wrangler login` args. When a callbackHost is given (the container case) we
 * also pass --browser=false so wrangler prints the auth URL instead of trying
 * to open a browser, and --callback-host so its OAuth callback server binds an
 * address reachable through `docker run -p 8976:8976`. Cloudflare's redirect_uri
 * is fixed to localhost:8976, so map that port on the host.
 */
export function buildLoginArgs({ callbackHost, callbackPort } = {}) {
  const args = ['login']
  if (callbackHost) {
    args.push('--browser=false', `--callback-host=${callbackHost}`)
    if (callbackPort) args.push(`--callback-port=${callbackPort}`)
  }
  return args
}
