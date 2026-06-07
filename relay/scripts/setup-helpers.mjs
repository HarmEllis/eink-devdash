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

/** The values setup writes/merges into the root .env for Docker + the API. */
export function relayEnvUpdates({ deviceUuid, deviceToken, relayPublishKey, workerUrl }) {
  return {
    DEVICE_TOKEN: deviceToken,
    DEVICE_UUID: deviceUuid,
    RELAY_ENABLED: 'true',
    RELAY_URL: workerUrl,
    RELAY_PUBLISH_KEY: relayPublishKey,
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
    'Docker (.env) was updated; start the publisher with:',
    '',
    '  docker compose up -d',
    '',
    'Keep these secret. Admin stats:',
    '',
    `  curl -H "Authorization: Bearer ${adminKey}" ${workerUrl}/admin/stats`,
  ].join('\n')
}
