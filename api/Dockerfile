FROM node:24-alpine AS builder
ARG APP_VERSION=
WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY . .
RUN npm run build

FROM node:24-alpine
ARG APP_VERSION=
WORKDIR /app
RUN apk add --no-cache bubblewrap
COPY package*.json ./
RUN npm ci --omit=dev
COPY --from=builder /app/dist ./dist
USER node
# Git tag the image was built from. /ota/manifest advertises this as
# latestVersion. Empty in local builds; the route then reports
# {otaEnabled:false} and logs a warning.
ENV APP_VERSION=${APP_VERSION}
EXPOSE 3000
CMD ["node", "dist/index.js"]
