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
          DEVICE_TOKENS: JSON.stringify({
            '11111111-1111-4111-8111-111111111111': 'device-test',
          }),
        },
      },
    }),
  ],
})
