FROM node:24-alpine AS builder
WORKDIR /app
COPY package*.json ./
COPY vendor ./vendor
RUN npm ci
COPY . .
RUN npm run build

FROM node:24-alpine
WORKDIR /app
COPY package*.json ./
COPY vendor ./vendor
RUN npm ci --omit=dev
COPY --from=builder /app/dist ./dist
USER node
EXPOSE 3000
CMD ["node", "dist/index.js"]
