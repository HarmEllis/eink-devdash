import { cloudflareTest } from '@cloudflare/vitest-pool-workers'
import { defineConfig } from 'vitest/config'

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: './wrangler.toml' },
      miniflare: {
        bindings: {
          RELAY_PUBLISH_KEY: 'publish-test',
          ADMIN_KEY: 'admin-test',
          DEVICE_IDENTITY_66666666666646668666666666666666: JSON.stringify({
            deviceToken: 'device-one',
            publishKey: 'publish-one',
            adminKey: 'admin-one',
          }),
          DEVICE_IDENTITY_77777777777747778777777777777777: JSON.stringify({
            deviceToken: 'device-two',
            publishKey: 'publish-two',
            adminKey: 'admin-two',
          }),
          DEVICE_TOKENS: JSON.stringify({
            '11111111-1111-4111-8111-111111111111': 'device-test',
            '22222222-2222-4222-8222-222222222222': 'device-test',
            '33333333-3333-4333-8333-333333333333': 'device-test',
            '44444444-4444-4444-8444-444444444444': 'device-test',
            '55555555-5555-4555-8555-555555555555': 'device-test',
          }),
          ACK_TIMEOUT_MS: '20',
          DASHBOARD_RESPONSE_DEADLINE_MS: '250',
          MANIFEST_RESPONSE_DEADLINE_MS: '100',
        },
      },
    }),
  ],
})
