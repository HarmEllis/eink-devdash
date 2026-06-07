#!/usr/bin/env node
// One-command self-host setup for the Cloudflare relay.
//
//   cd relay && npm install && npm run setup
//
// End to end: ensure wrangler is authenticated, generate a fresh device
// identity, deploy the Worker, push its secrets, merge the root .env that
// Docker + the API consume, and print provisioning details (URL + token + QR)
// for the firmware captive portal. No secret is ever typed or copied by hand.

import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { chmod, readFile, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

import {
  buildRelayDashboardUrl,
  formatProvisioningSummary,
  generateIdentity,
  mergeEnv,
  parseWorkerUrl,
  relayEnvUpdates,
} from './setup-helpers.mjs'

const scriptDir = dirname(fileURLToPath(import.meta.url))
const relayDir = join(scriptDir, '..')
const rootDir = join(relayDir, '..')
const rootEnvPath = join(rootDir, '.env')

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

  step('Not authenticated - opening Cloudflare login in your browser...')
  const login = await runWrangler(['login'], { interactive: true })
  if (login.code !== 0) {
    fail('wrangler login failed. Re-run "npm run setup" once you are logged in.')
  }
}

async function deployWorker() {
  step('Deploying the relay Worker (wrangler deploy)...')
  const result = await runWrangler(['deploy'])
  process.stdout.write(result.stdout)
  if (result.code !== 0) {
    process.stderr.write(result.stderr)
    fail('wrangler deploy failed. Check the output above (account not ready? quota?).')
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
  step('Pushing Worker secrets (via stdin, never argv)...')
  await pushSecret('DEVICE_UUID', identity.deviceUuid)
  await pushSecret('DEVICE_TOKEN', identity.deviceToken)
  await pushSecret('RELAY_PUBLISH_KEY', identity.relayPublishKey)
  await pushSecret('ADMIN_KEY', identity.adminKey)
}

async function updateRootEnv(identity, workerUrl) {
  step(`Updating ${rootEnvPath} (merge, not overwrite)...`)
  let existing = ''
  if (existsSync(rootEnvPath)) {
    existing = await readFile(rootEnvPath, 'utf8')
  }
  const merged = mergeEnv(existing, relayEnvUpdates({ ...identity, workerUrl }))
  await writeFile(rootEnvPath, merged)
  // writeFile's mode only applies on creation; force 0600 so a pre-existing
  // world-readable .env does not keep leaking the secrets we just wrote.
  await chmod(rootEnvPath, 0o600)
  console.log('  .env updated (DEVICE_TOKEN, DEVICE_UUID, RELAY_* set; mode 0600).')
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

  let qrcode
  try {
    qrcode = (await import('qrcode-terminal')).default
  } catch {
    qrcode = null
  }

  // Two QR codes so neither field has to be transcribed by hand: one for the
  // captive portal's API URL field, one for the device token field. A single
  // combined provisioning URL is a planned firmware-side enhancement.
  console.log('\nScan for the API URL field:\n')
  if (qrcode) qrcode.generate(relayDashboardUrl, { small: true })
  else console.log('  (install qrcode-terminal to render a QR here)')

  console.log('\nScan for the device token field:\n')
  if (qrcode) qrcode.generate(identity.deviceToken, { small: true })
  else console.log('  (install qrcode-terminal to render a QR here)')

  console.log(divider + '\n')
}

async function main() {
  await ensureWranglerInstalled()
  await ensureAuthenticated()

  step('Generating a fresh device identity...')
  const identity = generateIdentity()
  console.log(`  DEVICE_UUID: ${identity.deviceUuid}`)

  const workerUrl = await deployWorker()
  await pushSecrets(identity)
  await updateRootEnv(identity, workerUrl)
  await printProvisioning(identity, workerUrl)

  console.log('Done. Next: "docker compose up -d", then provision the firmware above.')
}

main().catch((err) => fail(err?.stack ?? String(err)))
