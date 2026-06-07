# Self-contained relay provisioning image. Bundles the Worker source + wrangler
# + the setup script so a user can deploy the relay to their own Cloudflare
# account with a single `docker run` — no clone, no Node, no wrangler install.
# See the README "Setup without cloning (Docker)" section for usage.
FROM node:24-slim

WORKDIR /relay

COPY package.json package-lock.json ./
RUN npm ci

# Worker source needed for `wrangler deploy`, plus the setup orchestrator.
COPY wrangler.toml tsconfig.json ./
COPY src ./src
COPY scripts ./scripts

# Wrangler writes scratch state under the project dir and its cache; make those
# writable regardless of the runtime UID, so callers can pass
# `--user "$(id -u):$(id -g)"` and have the mounted /out/.env owned by the host
# user instead of root.
RUN mkdir -p /out /relay/.wrangler /relay/node_modules/.cache \
 && chmod -R 0777 /out /relay/.wrangler /relay/node_modules/.cache

ENV HOME=/tmp \
    XDG_CONFIG_HOME=/tmp/.config \
    WRANGLER_HOME=/tmp/.wrangler \
    RELAY_SETUP_ENV_OUT=/out/.env \
    RELAY_SETUP_CALLBACK_HOST=0.0.0.0

# Default to the unprivileged user baked into node:* images. Override with
# `--user "$(id -u):$(id -g)"` on Linux so /out/.env is owned by you.
USER node

VOLUME ["/out"]
ENTRYPOINT ["node", "scripts/setup.mjs"]
