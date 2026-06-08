#!/usr/bin/env node
// Reprint provisioning QR codes from the existing repository-root .env
// without deploying the relay or rotating credentials.

import { readFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { parseEnv } from 'node:util'
import { fileURLToPath } from 'node:url'

import qrcode from 'qrcode-terminal'

import { buildRelayDashboardUrl } from './setup-helpers.mjs'

const scriptDir = dirname(fileURLToPath(import.meta.url))
const envPath = process.env.RELAY_SETUP_ENV_OUT || join(scriptDir, '..', '..', '.env')

function fail(message) {
  console.error(`\nERROR: ${message}\n`)
  process.exit(1)
}

let env
try {
  env = parseEnv(await readFile(envPath, 'utf8'))
} catch (error) {
  fail(`Cannot read or parse ${envPath}: ${error.message}`)
}

const requiredKeys = ['RELAY_URL', 'DEVICE_UUID', 'DEVICE_TOKEN']
const missingKeys = requiredKeys.filter((key) => !env[key]?.trim())
if (missingKeys.length > 0) {
  fail(`Missing required value(s) in ${envPath}: ${missingKeys.join(', ')}`)
}

let relayDashboardUrl
try {
  relayDashboardUrl = buildRelayDashboardUrl(env.RELAY_URL, env.DEVICE_UUID)
} catch (error) {
  fail(`Invalid RELAY_URL in ${envPath}: ${error.message}`)
}

console.log('\nScan for the API URL field:\n')
console.log(`  ${relayDashboardUrl}\n`)
qrcode.generate(relayDashboardUrl, { small: true })

console.log('\nScan for the device token field:\n')
qrcode.generate(env.DEVICE_TOKEN, { small: true })
