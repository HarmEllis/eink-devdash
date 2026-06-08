#!/usr/bin/env node
// One-command self-host setup for the Cloudflare relay.
//
//   cd relay && npm install && npm run setup     # local
//   docker run ... eink-devdash-relay-setup       # no clone (see README)
//
// End to end: authenticate wrangler (CLOUDFLARE_API_TOKEN, else interactive
// login), create or reuse this machine's device identity, deploy the Worker,
// add per-device secrets, merge the .env that Docker + the API consume, and
// print provisioning details (URL + token + QR) for the firmware captive
// portal. No secret is ever typed or copied by hand.
//
// Env overrides (used by the container image):
//   RELAY_SETUP_ENV_OUT       where to write the merged .env (default repo .env)
//   RELAY_SETUP_CALLBACK_HOST  wrangler login --callback-host (e.g. 0.0.0.0)
//   RELAY_SETUP_CALLBACK_PORT  wrangler login --callback-port
//   RELAY_SETUP_PRINT_ENV=1    also echo the full .env block to stdout

import { spawn } from 'node:child_process'
import { randomBytes } from 'node:crypto'
import { constants as fsConstants, existsSync } from 'node:fs'
import { access, chmod, lstat, open, rename, unlink, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

import {
  buildLoginArgs,
  buildRelayDashboardUrl,
  chooseAuthMode,
  formatEnvBlock,
  formatProvisioningSummary,
  generateIdentity,
  identityFromEnv,
  mergeEnv,
  parseWorkerUrl,
  relayEnvUpdates,
  workerIdentitySecret,
  workerSecretName,
} from './setup-helpers.mjs'

const scriptDir = dirname(fileURLToPath(import.meta.url))
const relayDir = join(scriptDir, '..')
const rootDir = join(relayDir, '..')
// Default to the repo-root .env (local `npm run setup`). The container image
// sets RELAY_SETUP_ENV_OUT=/out/.env so the file lands on a mounted volume.
const envOutPath = process.env.RELAY_SETUP_ENV_OUT || join(rootDir, '.env')

const WRANGLER_BIN = join(relayDir, 'node_modules', '.bin', 'wrangler')

function fail(message) {
  console.error(`\nERROR: ${message}\n`)
  process.exit(1)
}

function step(message) {
  console.log(`\n> ${message}`)
}

/**
 * Run wrangler. Returns { code, stdout, stderr }. When opts.interactive is set
 * the child inherits the TTY (for `login`). When opts.input is set it is written
 * to stdin (for `secret put`, so the secret never appears in argv/process list).
 */
function runWrangler(args, opts = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(WRANGLER_BIN, args, {
      cwd: relayDir,
      stdio: opts.interactive
        ? 'inherit'
        : [opts.input != null ? 'pipe' : 'ignore', 'pipe', 'pipe'],
    })

    let stdout = ''
    let stderr = ''
    child.stdout?.on('data', (chunk) => { stdout += chunk })
    child.stderr?.on('data', (chunk) => { stderr += chunk })

    child.on('error', reject)
    child.on('close', (code) => resolve({ code, stdout, stderr }))

    if (opts.input != null) {
      child.stdin.write(opts.input)
      child.stdin.end()
    }
  })
}

async function ensureWranglerInstalled() {
  if (!existsSync(WRANGLER_BIN)) {
    fail('wrangler is not installed. Run "npm install" inside relay/ first.')
  }
}

async function ensureAuthenticated() {
  const mode = chooseAuthMode({
    hasToken: Boolean(process.env.CLOUDFLARE_API_TOKEN),
    isInteractive: process.stdin.isTTY === true,
  })

  if (mode === 'token') {
    // Don't gate on `wrangler whoami`: narrowly-scoped Workers tokens can deploy
    // but lack the user/account read that whoami needs. Let `wrangler deploy`
    // be the real auth gate and surface its error.
    step('Using CLOUDFLARE_API_TOKEN for authentication.')
    return
  }

  if (mode === 'unavailable') {
    fail(
      'No Cloudflare authentication available in this non-interactive run.\n'
      + '  Provide a token:  export CLOUDFLARE_API_TOKEN=... '
      + '(and CLOUDFLARE_ACCOUNT_ID if your token spans multiple accounts)\n'
      + '  Or log in:        re-run with a TTY, e.g. `docker run -it -p 8976:8976 ...`',
    )
  }

  // mode === 'login': interactive OAuth. Short-circuit if already logged in so
  // re-runs don't force a fresh login.
  step('Checking Cloudflare authentication (wrangler whoami)...')
  let result
  try {
    result = await runWrangler(['whoami'])
  } catch (err) {
    fail(`Could not run wrangler: ${err.message}`)
  }

  const output = `${result.stdout}\n${result.stderr}`
  const loggedIn = result.code === 0 && !/not authenticated|not logged in/i.test(output)
  if (loggedIn) {
    console.log('  Already authenticated.')
    return
  }

  step('Not authenticated - starting Cloudflare login...')
  const args = buildLoginArgs({
    callbackHost: process.env.RELAY_SETUP_CALLBACK_HOST,
    callbackPort: process.env.RELAY_SETUP_CALLBACK_PORT,
  })
  const login = await runWrangler(args, { interactive: true })
  if (login.code !== 0) {
    fail('wrangler login failed. Re-run once you are logged in.')
  }
}

async function deployWorker() {
  step('Deploying the relay Worker (wrangler deploy)...')
  const result = await runWrangler(['deploy'])
  process.stdout.write(result.stdout)
  if (result.code !== 0) {
    process.stderr.write(result.stderr)
    const combined = `${result.stdout}\n${result.stderr}`
    if (/workers\.dev|subdomain|register/i.test(combined)) {
      fail(
        'wrangler deploy failed: your Cloudflare account may not have a workers.dev\n'
        + '  subdomain yet. Register one once at https://dash.cloudflare.com\n'
        + '  (Workers & Pages -> set up a subdomain), then re-run.',
      )
    }
    fail('wrangler deploy failed. Check the output above (token scope? account id? quota?).')
  }

  const workerUrl = parseWorkerUrl(result.stdout) ?? parseWorkerUrl(result.stderr)
  if (!workerUrl) {
    fail(
      'Deploy succeeded but no *.workers.dev URL was detected (custom domain or route?).\n'
      + '  This script cannot continue automatically. Finish via the manual path in the\n'
      + '  README ("Manual / advanced deploy"), which lets you set RELAY_URL yourself.',
    )
  }
  console.log(`  Worker URL: ${workerUrl}`)
  return workerUrl
}

async function pushSecret(name, value) {
  const result = await runWrangler(['secret', 'put', name], { input: `${value}\n` })
  if (result.code !== 0) {
    process.stderr.write(result.stderr)
    fail(`Failed to set secret ${name}.`)
  }
  console.log(`  set ${name}`)
}

async function pushSecrets(identity) {
  step('Adding this device identity to the Worker (via stdin, never argv)...')
  await pushSecret(workerSecretName(identity.deviceUuid), workerIdentitySecret(identity))
}

async function preflightEnvOut() {
  // Run BEFORE deploy/secret registration and return the existing file contents, so
  // an unwritable dir, a symlinked/unreadable target, or a non-regular file
  // fails up front rather than after we have already updated the Worker.
  const dir = dirname(envOutPath)
  try {
    await access(dir, fsConstants.W_OK)
  } catch {
    fail(
      `Output directory ${dir} is not writable.\n`
      + '  Mount a writable volume at /out, or set RELAY_SETUP_ENV_OUT to a writable path.',
    )
  }

  if (!existsSync(envOutPath)) {
    return ''
  }

  // lstat (no-follow): refuse a symlink or special file up front.
  const stat = await lstat(envOutPath)
  if (!stat.isFile()) {
    fail(`${envOutPath} is not a regular file (symlink or special file); refusing to write.`)
  }
  // Read now (O_NOFOLLOW) and reuse for the merge — a single read avoids a
  // read/write TOCTOU and surfaces an unreadable file before we deploy.
  let handle
  try {
    handle = await open(envOutPath, fsConstants.O_RDONLY | fsConstants.O_NOFOLLOW)
    return await handle.readFile('utf8')
  } catch (err) {
    fail(`Cannot read ${envOutPath} (symlink or unreadable): ${err.message}`)
  } finally {
    await handle?.close()
  }
}

async function writeEnvFile(identity, workerUrl, existing) {
  step(`Updating ${envOutPath} (merge, not overwrite)...`)

  const merged = mergeEnv(existing, relayEnvUpdates({ ...identity, workerUrl }))
  // Exclusive, randomized temp sibling ('wx' = O_CREAT|O_EXCL, never follows a
  // symlink and never collides), then atomic rename over the target. rename()
  // does not follow symlinks, so a symlinked target is replaced by our regular
  // file instead of written through.
  const tmpPath = `${envOutPath}.tmp-${randomBytes(8).toString('hex')}`
  try {
    await writeFile(tmpPath, merged, { flag: 'wx', mode: 0o600 })
    await rename(tmpPath, envOutPath)
  } catch (err) {
    await unlink(tmpPath).catch(() => {})
    fail(`Failed to write ${envOutPath}: ${err.message}`)
  }
  // Best-effort: chmod is a no-op on some Docker Desktop bind mounts.
  try {
    await chmod(envOutPath, 0o600)
  } catch {
    /* ignore */
  }
  console.log('  Wrote this machine\'s DEVICE_TOKEN, DEVICE_UUID, and RELAY_* values.')
}

async function printProvisioning(identity, workerUrl) {
  const relayDashboardUrl = buildRelayDashboardUrl(workerUrl, identity.deviceUuid)
  const divider = '-'.repeat(72)
  console.log('\n' + divider)
  console.log(formatProvisioningSummary({
    relayDashboardUrl,
    deviceToken: identity.deviceToken,
    adminKey: identity.adminKey,
    workerUrl,
  }))

  // The summary above intentionally echoes the device token (needed to
  // provision the firmware) and admin key (needed for /admin/stats). The .env
  // file is the primary machine-readable channel; RELAY_SETUP_PRINT_ENV=1 also
  // echoes that full block, which adds RELAY_PUBLISH_KEY.
  if (process.env.RELAY_SETUP_PRINT_ENV === '1') {
    console.log('\n.env (keep secret):\n')
    console.log(formatEnvBlock(identity, workerUrl))
  }

  // QR codes only make sense on an interactive terminal.
  if (process.stdout.isTTY) {
    let qrcode
    try {
      qrcode = (await import('qrcode-terminal')).default
    } catch {
      qrcode = null
    }

    // Two QR codes so neither field has to be transcribed by hand: one for the
    // captive portal's API URL field, one for the device token field.
    console.log('\nScan for the API URL field:\n')
    if (qrcode) qrcode.generate(relayDashboardUrl, { small: true })
    else console.log('  (install qrcode-terminal to render a QR here)')

    console.log('\nScan for the device token field:\n')
    if (qrcode) qrcode.generate(identity.deviceToken, { small: true })
    else console.log('  (install qrcode-terminal to render a QR here)')
  }

  console.log(divider + '\n')
}

async function main() {
  await ensureWranglerInstalled()
  await ensureAuthenticated()
  const existingEnv = await preflightEnvOut()

  let identity
  try {
    identity = identityFromEnv(existingEnv)
  } catch (error) {
    fail(error.message)
  }
  if (identity) {
    step('Reusing this machine\'s existing device identity...')
  } else {
    step('Generating a fresh device identity...')
    identity = generateIdentity()
  }
  console.log(`  DEVICE_UUID: ${identity.deviceUuid}`)

  const workerUrl = await deployWorker()
  await pushSecrets(identity)
  await writeEnvFile(identity, workerUrl, existingEnv)
  await printProvisioning(identity, workerUrl)

  console.log('Done. Start the publisher with the generated .env, then provision the firmware above.')
}

main().catch((err) => fail(err?.stack ?? String(err)))
