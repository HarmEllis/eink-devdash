import { test } from 'node:test'
import assert from 'node:assert/strict'
import Fastify from 'fastify'

import {
  buildDownloadUrl,
  buildManifest,
  getRepoSlug,
  otaRoute,
} from './ota.js'

test('repo slug is hard-coded to the canonical fork', () => {
  // A wrong fork must not silently advertise the wrong download URL.
  // Bumping the slug is intentionally a code change, not env.
  assert.equal(getRepoSlug(), 'HarmEllis/eink-devdash')
})

test('buildDownloadUrl points at the GitHub Releases asset', () => {
  assert.equal(
    buildDownloadUrl('v0.2.0'),
    'https://github.com/HarmEllis/eink-devdash/releases/download/v0.2.0/eink-devdash.bin',
  )
})

test('buildManifest returns {otaEnabled:false} only when OTA disabled', () => {
  assert.deepEqual(
    buildManifest({ otaEnabled: false, appVersion: 'v0.2.0' }),
    { otaEnabled: false },
  )
})

test('buildManifest returns {otaEnabled:false} when APP_VERSION is missing', () => {
  assert.deepEqual(
    buildManifest({ otaEnabled: true, appVersion: '' }),
    { otaEnabled: false },
  )
})

test('buildManifest returns the full manifest when enabled + version present', () => {
  assert.deepEqual(
    buildManifest({ otaEnabled: true, appVersion: 'v0.2.0' }),
    {
      otaEnabled: true,
      latestVersion: 'v0.2.0',
      downloadUrl:
        'https://github.com/HarmEllis/eink-devdash/releases/download/v0.2.0/eink-devdash.bin',
    },
  )
})

test('/ota/manifest is bearer-gated by the global auth hook', async () => {
  const originalDeviceToken = process.env.DEVICE_TOKEN
  const originalOtaEnabled = process.env.OTA_ENABLED
  const originalAppVersion = process.env.APP_VERSION
  try {
    process.env.DEVICE_TOKEN = 'test-token'
    process.env.OTA_ENABLED = 'true'
    process.env.APP_VERSION = 'v0.2.0'

    // The auth hook lives in src/index.ts; reproduce it here so the unit
    // test exercises the same contract without booting the whole index.
    const app = Fastify()
    try {
      app.addHook('onRequest', async (req, reply) => {
        if (req.url === '/health') return
        const auth = req.headers.authorization
        if (!auth || auth !== `Bearer ${process.env.DEVICE_TOKEN}`) {
          return reply.code(401).send({ error: 'Unauthorized' })
        }
      })
      await app.register(otaRoute)

      const unauthed = await app.inject({
        method: 'GET',
        url: '/ota/manifest',
      })
      assert.equal(unauthed.statusCode, 401)

      const authed = await app.inject({
        method: 'GET',
        url: '/ota/manifest',
        headers: { authorization: 'Bearer test-token' },
      })
      assert.equal(authed.statusCode, 200)
      assert.deepEqual(JSON.parse(authed.body), {
        otaEnabled: true,
        latestVersion: 'v0.2.0',
        downloadUrl:
          'https://github.com/HarmEllis/eink-devdash/releases/download/v0.2.0/eink-devdash.bin',
      })
    } finally {
      await app.close()
    }
  } finally {
    if (originalDeviceToken === undefined) delete process.env.DEVICE_TOKEN
    else process.env.DEVICE_TOKEN = originalDeviceToken
    if (originalOtaEnabled === undefined) delete process.env.OTA_ENABLED
    else process.env.OTA_ENABLED = originalOtaEnabled
    if (originalAppVersion === undefined) delete process.env.APP_VERSION
    else process.env.APP_VERSION = originalAppVersion
  }
})
