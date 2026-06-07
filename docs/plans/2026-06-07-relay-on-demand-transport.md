# Plan: On-demand request/response over the Cloudflare relay

## Goal

Make a relay API profile behave like a direct API profile: when the device
fetches `/d/<uuid>/dashboard` (or `/d/<uuid>/ota/manifest`), it receives data
the API generated **at fetch time**, not a snapshot the API happened to push
minutes earlier. Today the relay is store-and-forward: the API pushes a
dashboard every `RELAY_PUBLISH_INTERVAL_MS` (default 5 min), the relay marks it
stale after `STALE_AFTER_MS` (2 min), and the device — which wakes every ~3 min
— almost always reads `stale: true`. The firmware then conflates `stale` with
`offline`, so it shows an `OFFLINE` header (dropping the Wi-Fi/API/sync info)
and forces a full e-paper refresh every cycle.

This plan adds **on-demand request/response over the relay's existing outbound
WebSocket** (chosen by the maintainer as "option 2"). The API still dials out
to the relay (the API is behind home NAT with no public ingress, so a
pull-through proxy is impossible — only the API can open the connection). The
relay rides that already-open socket in the reverse direction: a device fetch
triggers a `request` frame to the API, the API builds a fresh payload and sends
a `response`, and the relay returns it.

## Relationship to the OTA manifest plan

This **supersedes the transport mechanism** in
`docs/plans/2026-06-06-relay-ota-manifest.md`. On-demand pull unifies dashboard
and manifest delivery, so the separate manifest heartbeat
(`MANIFEST_HEARTBEAT_MS`) and Worker TTL (`MANIFEST_TTL_MS`) are **dropped** —
they only bounded staleness in the push model, which no longer exists.

**Kept verbatim from the OTA plan** (orthogonal firmware trust-anchor, already
implemented + host-tested this session):

- `firmware/main/ota_version.{c,h}` — `ota_download_url_is_canonical` (exact
  canonical GitHub URL pinning) and `ota_version_is_newer` (upgrade-only,
  fail-closed `vMAJOR.MINOR.PATCH` parser). No ESP-IDF deps.
- `firmware/main/ota_client.c` — upgrade-only gate + canonical-URL rejection
  replacing the old `versions_match` equality gate; the cert-bundle attach on
  the manifest fetch and the `404 → no-op` fallback stay.
- `api/src/routes/ota.ts` — `getOtaManifest()` single source of truth +
  `APP_VERSION` canonical validation (`^v\d+\.\d+\.\d+$`, bounded to uint32,
  no leading zeros; malformed → `{ otaEnabled: false }` + warn).

## Security: freshness, replay, and backpressure

- **Manifest has no fallback cache.** Because the manifest can trigger a
  firmware install, a cached/replayed manifest is a security risk: an offline
  API plus an old stored manifest could push a device that is further behind
  onto an older, vulnerable-but-canonical release (upgrade-only does not stop
  this — the stored manifest is "newer" than the device). The relay therefore
  serves the manifest **on demand only**: if it cannot obtain a fresh manifest
  (no publisher, timeout, or error response) it returns `404`. The firmware
  already treats a manifest `404` as a graceful no-op. The API builds the
  manifest from its current `APP_VERSION` on every request, so it is never
  stale.
- **Dashboard may keep an indefinite fallback snapshot.** It is display-only
  data with no security impact; serving last-known data with `stale: true` is a
  feature, not a replay risk.
- **Bounded fan-out / backpressure.** Every authenticated device fetch would
  otherwise start an independent dashboard build that fans out to the provider
  APIs (GitHub/Anthropic/OpenAI). A replayed device token or concurrent firmware
  retries could exhaust the home API and upstream rate limits. Three layers
  bound this — coalescing handles *concurrency*, a min-refresh window handles
  *rate*:
  - **Relay coalescing**: at most **one in-flight `request` per resource per
    device** (a resource-keyed in-flight map). Concurrent device fetches for the
    same resource attach to the single in-flight request and share its
    finalized result; they do not each emit a `request` frame.
  - **Relay attempt limiter** (rate limit; coalescing alone only caps
    concurrency, not sequential abuse): a per-resource `nextAllowedAt` cooldown
    of `MIN_REFRESH_MS = 60_000`, **persisted in Durable Object storage** and
    set **atomically just before emitting the `request`** (so it survives
    hibernation — a hibernatable DO may discard in-memory state after ~10 s idle
    — and is enforced after **both** success and failure; a fast build failure
    cannot reset the limiter). While `Date.now() < nextAllowedAt[resource]` the
    relay does **not** emit a new `request`:
    - dashboard → serve the stored snapshot (stale flag computed from its own
      `lastPublishAt` age, independent of the limiter);
    - manifest → return `404` (graceful firmware skip; never cached, so
      unreplayable). This caps the per-device build/fan-out rate to ≤1 build per
      60 s per resource no matter how fast a (possibly stolen) token polls, even
      across forced instance resets. `MIN_REFRESH_MS` is set **at least as high
      as the shortest provider cache window** (the GitHub adapter's 60 s) so that
      even an at-limit attacker cannot out-pace the provider caches — combined
      with the reused adapter registry (see wiring), back-to-back builds hit the
      caches instead of re-fanning out. 60 s is still below any sane device
      refresh interval (minutes), so normal operation always builds fresh. The
      dashboard fresh-cache age (`lastPublishAt`) is kept **separate** from this
      attempt limiter (`nextAllowedAt`). Lifecycle ref:
      https://developers.cloudflare.com/durable-objects/concepts/durable-object-lifecycle/
  - **API single build (process-wide coordinator)**: a single in-flight
    dashboard-build promise held by **one process-wide coordinator** that is the
    sole entry point for **every** build path — the direct Fastify `/dashboard`
    route, the relay periodic push, the relay seed, and on-demand `request`
    handling all call it. Concurrent callers share the one in-flight build, so a
    single API process never runs two concurrent dashboard builds (the previous
    wiring let the direct route build independently of the relay — see API
    wiring). This bounds *intra-process* concurrency; the separate per-publisher
    nature of cross-publisher failover builds is bounded by `MAX_FAILOVER` and
    documented honestly in the relay section.

## Non-goals

- No pull-through HTTP proxy (the API has no public ingress; impossible by
  design).
- No change to where firmware binaries live (still GitHub Releases).
- No OTA downgrade (upgrade-only stays).
- The direct LAN/HTTPS API profile is untouched: it already serves live data.

## Design

### Frame protocol (relay ⇄ API over the publisher WebSocket)

Publisher (API) → relay:

- `{ type: 'hello'; capabilities: string[] }` — sent **once immediately on
  open**, before the dashboard seed, to advertise which **resources** the
  publisher can serve on-demand. Capabilities are **resource-specific** (round-10):
  a publisher that can build dashboards advertises `'dashboard'`, and only one that
  actually wires `getManifest` adds `'manifest'` (so `capabilities` is e.g.
  `['dashboard','manifest']`, or `['dashboard']` for a manifest-less build). The
  relay persists it on the socket via `ws.serializeAttachment({ epoch, caps })`
  (hibernation-safe). An older API that predates this plan never sends `hello`; a
  socket with empty `caps` is **legacy** and is **never** sent a `request`. A
  socket is eligible for a given resource's `request` **only if `caps` include
  that resource** — so a dashboard-only publisher is never asked for a manifest
  (which would otherwise produce an undefined/malformed payload and wrongly
  deprioritize it). New.
- `{ type: 'dashboard'; payload }` — push a dashboard snapshot (fallback cache +
  WS keepalive). Unchanged.
- `{ type: 'request-ack'; id: string }` — sent **synchronously on receiving a
  `request`**, before the (possibly slow) build runs, to prove the socket is
  alive and processing this `id`. Lets the relay distinguish a *dead/legacy*
  socket (no ack) from a *slow-but-healthy* build (ack, then a later `response`).
  New. (Distinct from the relay→publisher dashboard-push `ack` below; this one
  carries an `id` and travels publisher→relay.)
- `{ type: 'response'; id: string; resource: 'dashboard' | 'manifest'; payload? ; error? }`
  — answer to a relay `request`. New.
- `{ type: 'ping' }` — unchanged (relay replies `pong`).

Relay → publisher (API):

- `{ type: 'request'; id: string; resource: 'dashboard' | 'manifest' }` — ask
  the API to build a fresh payload. Sent **only to a socket whose `caps` include
  the requested `resource`**. New.
- `{ type: 'ack' | 'pong' | 'error' }` — unchanged (`ack` here is the relay
  acknowledging a `dashboard` push; it carries no `id`).

`id` is `crypto.randomUUID()`; `resource` is derived from the device path.
There is **no** `manifest` push frame: the manifest is never cached.

### Relay — `relay/src/index.ts`

**Worker-level router.** The outer `export default { fetch }` dispatcher today
matches only `/^\/d\/([^/]+)(?:\/dashboard)?$/` — `/d/<uuid>/ota/manifest` would
hit the Worker's generic `404` and never reach the Durable Object. Extend the
regex to `/^\/d\/([^/]+)(?:\/dashboard|\/ota\/manifest)?$/` and route every match
through `dashboardRequest`, which already strips client-supplied internal
headers and injects the trusted `x-relay-device-token` / `x-relay-uuid` /
`x-relay-stale-after-ms`. The DO then derives the resource from the path. The
full-Worker-route tests must cover the manifest path through `worker.fetch`.

**Publisher selection — newest-epoch wins, never kick (no lease, no terminal
close).** The relay does **not** close or "retire" publishers, because any
close-and-stop scheme is either a reconnect oscillation (round-4) or, with a
terminal stop, a persistent denial of service: a brief/malicious replacement
using the global publish key would leave the legitimate publisher permanently
offline after it disconnects. Instead:

- `acceptPublisher` tags each accepted socket with a **strictly increasing
  epoch** drawn from a persisted counter (`epoch = (await
  storage.get('publisherEpoch') ?? 0) + 1; await storage.put('publisherEpoch',
  epoch)`) and the negotiated capabilities, stored together via
  `ws.serializeAttachment({ epoch, caps })` (hibernation-safe). `caps` starts
  empty and is rewritten when the `hello` frame arrives (a socket that never
  sends `hello` keeps `caps: []` → legacy). `Date.now()` is **not** used for the
  epoch — two sockets in the same millisecond or a clock adjustment would make
  "newest" ambiguous. Multiple publisher sockets may coexist; the relay never
  closes one.
- **Resource-gated, health-ordered candidate list.** For the requested resource
  R, the send path builds the candidate list from every `OPEN` socket whose stored
  `caps` include **R** (`getWebSockets()` may include `CLOSING` sockets —
  https://developers.cloudflare.com/durable-objects/api/state/ — and order is
  unspecified, so never assume `[0]`); legacy (no-`hello`) and not-R-capable
  sockets are excluded. The list is sorted by **(epoch ∉ deprioritized[R] first,
  then highest-epoch first)** — deprioritization is tracked **per resource** (see
  below) — so a socket that is bad for *manifest* is not penalised for *dashboard*,
  and a known-bad newest socket for R is tried *after* a healthy older one while
  still defaulting to newest when none are flagged. An empty list → the relay emits
  no `request` and falls back immediately (so a Worker deploy in front of a
  still-old API never waits the deadline — it serves the snapshot / `404` at once,
  which is what the older firmware's shorter HTTP timeout needs). Only the chosen
  socket receives a `request` frame, so there is no duplicate fan-out.
- **Two-layer failover: in-request (dead socket) + cross-request
  (deprioritization) — no permanent blackhole, hibernation-safe.** A purely
  in-request scheme cannot cover an *ACK-then-hang/error* socket (a second full
  build does not fit under the firmware timeout — see deadline budget), and a
  purely cross-request scheme with a long TTL would blackhole the newest socket
  for a whole poll interval (round-7). The two layers together close both gaps:
  1. **In-request ACK failover (dead/no-ack socket).** Send `request` to
     `candidates[0]` and arm an `ACK_TIMEOUT_MS` (≈2 000) timer inside the
     resource deadline. A matching `request-ack` first → the socket is alive and
     building: cancel the ack timer and await the `response`/deadline (a
     slow-but-healthy build is **never** failed over — the dead-vs-slow
     distinction the ack buys). Ack timer fires first (or the `send` threw) →
     treat that socket as dead *for this request*: deprioritize its epoch (step 3),
     advance to the next candidate, re-arm the ack timer, resend the **same `id`**.
     Capped at **`MAX_FAILOVER = 2`** sends total and never past the deadline. This
     reaches a healthy older publisher **within the same request** when the newest
     is outright dead.
  2. **Response-phase failure → fall back now, recover next request.** If a
     candidate **acked** but then neither responds before the deadline **nor**
     sends a usable `response` (an explicit `error`, or a deadline expiry after
     ack), the relay does **not** start a second full build in this request (it
     would not fit the firmware timeout). It serves fallback for *this* request
     and **deprioritizes the offending epoch** (step 3) so the **next** poll
     selects a healthy older publisher first. "No permanent blackhole" is
     guaranteed across requests: at most one polling cycle is degraded before
     traffic moves off the bad publisher.
  3. **Persisted, per-resource, evidence-cleared deprioritization (no wall-clock
     re-promotion, no cross-resource rehab).** On any failover/response-phase
     failure for resource **R**, add the offending **epoch** to a persisted
     per-resource set `deprioritized[R]` in **DO storage** (survives hibernation).
     Selection for R sorts a deprioritized epoch to the back (not excluded — still
     reachable if it is the only R-capable candidate). Two properties make this
     correct where earlier designs failed:
     - **No wall-clock re-promotion (round-9):** a deprioritized epoch is **never**
       promoted back to the foreground because time elapsed. A TTL would re-promote
       an ACK-then-hang publisher ahead of the healthy older one, and since
       response-phase failures deliberately do not re-fail-over inside the same
       request, that poll would fall back again — recurring every poll once the
       poll interval ≥ TTL.
     - **Per-resource, request-proven clearing (round-10):** the flag for
       `(R, epoch)` clears **only** on a **successful on-demand `response` for R**
       from that epoch (proof that *that* path is healthy), or when the socket
       **closes/reconnects** (its epoch is gone; a reconnect gets a **new, higher,
       un-flagged** epoch — the normal recovery path for a restarted/redeployed
       publisher). A periodic `dashboard` **push does NOT clear** any flag: a push
       proves only that the socket can emit a dashboard, **not** that its
       request/response path — let alone manifest generation — is healthy, so using
       it to rehabilitate would let a publisher broken on manifest (or on-demand)
       keep regaining priority and cause recurring fallbacks.
     A deprioritized epoch is still **reachable when it is the only R-capable
     candidate** (sorted to back but selected), so a transient single-publisher
     failure self-heals on its next successful response; when a healthier
     higher-epoch candidate exists, the bad epoch simply stays out of the way until
     it reconnects, with no device impact and no probe needed. Entries for epochs
     whose socket is gone are **pruned** during selection so the sets cannot grow
     unbounded. Because correctness routing comes from layers 1–2 every request and
     the persisted set only *reorders* (clearing solely on real, resource-specific
     health evidence), the round-7 (short-TTL-lost), round-9 (wall-clock
     re-promotion) and round-10 (cross-resource/push rehab) failure modes all
     cannot occur.
  The attempt-limiter cooldown is armed **once** per owner request (not per
  failover send). Cross-*publisher* build counting is addressed honestly below
  (the dead-socket ack-failover case can start up to `MAX_FAILOVER` builds on
  distinct publishers); the cooldown bounds owner *requests*, not executions on
  separate publishers.
- Each `inflight` entry tracks the **current candidate socket** it last sent the
  `request` on (`entry.socket`, updated on each failover send); a `response` or
  `request-ack` is accepted only when `entry.socket === ws` **and** `id` matches
  (rejects a *cross-socket* / stale-candidate reply). A matching response on the
  current candidate stays valid **even if a newer publisher has since connected**
  — accepting a newer socket affects only *future* requests, never an in-flight
  one. `webSocketClose/Error(ws)` on the current candidate triggers an immediate
  failover attempt (or `null`/fallback if candidates/`MAX_FAILOVER` are
  exhausted).
- The API client needs **no** special close handling: any close → reconnect with
  backoff (existing). A deploy overlap (old + new container) converges naturally
  — the new socket has the higher epoch and gets the requests; the old one
  receives none and closes on shutdown. After any disconnect the legitimate
  publisher simply reconnects, so there is no permanent-offline failure mode.
- A hostile publisher holding the global key can attract requests **only while
  actively connected** (inherent to a shared key), and nothing it serves persists
  past its disconnect (no manifest cache). The firmware trust anchor bounds — but
  does **not** eliminate — the OTA damage, and the bound must be stated honestly:
  - **What the trust anchor blocks:** the canonical-URL pin constrains the
    accepted `downloadUrl` to the exact
    `github.com/HarmEllis/eink-devdash/releases/...` **repository / tag / asset
    path** for the named version, and upgrade-only rejects anything ≤ the device's
    running version. An attacker therefore **cannot** point the device at an
    arbitrary attacker-hosted host/path, nor downgrade it.
  - **What it does NOT block — and the honest authenticity limit:** URL pinning
    constrains *only the path*, not the *bytes*. The firmware verifies **no
    release signature and no independently trusted digest**; it trusts whatever
    asset currently sits at that canonical path, fetched over TLS. Binary
    authenticity therefore rests entirely on **TLS-to-github.com + GitHub
    repository access control**, not on a firmware-verified signature — anyone who
    can write to the repository (or a compromised maintainer/CI token) can
    **replace** the asset bytes at a canonical path. So a pinned URL is **not** a
    guarantee of a "genuine" build; it only guarantees the *path*. And within the
    pin, the publish-key holder can still name **any real, canonical release newer
    than the device** — including an *older, already-superseded-but-still-newer*
    release with a known vulnerability ("newer than the device" ≠ "the latest
    release"; a lagging device can be steered onto a stale-vulnerable build). So
    the publish key is, accurately, **the authority to select any eligible
    official firmware path for a device**, not merely the authority to cause a
    `404`, and the OTA chain provides **no cryptographic binary-authenticity
    guarantee** today. (A non-existent version still degrades to download `404` →
    graceful skip; that is the *failure* mode, not a security *bound*.)
  - **Compromise response:** treat `RELAY_PUBLISH_KEY` as a sensitive credential —
    rotate it on suspected compromise (documented in the ADR/README); rotation
    cuts off the attacker immediately because nothing is cached. The **only** way
    to make binary authenticity an actual invariant (so neither a relay-key holder
    nor a repository-write compromise can install unverified bytes) is a
    **firmware-verified signature**: ship an embedded public key and have the
    firmware verify a signature over the manifest (and ideally the binary digest)
    before installing. That is a real future hardening step, recorded as an open
    option in ADR 0007 — **not** something the current canonical-URL pin provides
    — and is out of scope for this transport change.

**In-flight correlation state** (instance fields, reset in the constructor):

```ts
type Settled =
  | { kind: 'fresh'; body: Record<string, unknown> }
  | { kind: 'fallback' }
type Inflight = {
  id: string
  resource: 'dashboard' | 'manifest'
  candidates: WebSocket[]     // resource-gated, health-ordered (snapshot)
  epochs: number[]            // candidates[i]'s epoch (for deprioritize/clear)
  attempt: number             // index into candidates; bounded by MAX_FAILOVER
  generation: number          // bumped per send; stale callbacks are ignored
  socket: WebSocket           // current candidate (candidates[attempt])
  acked: boolean              // current candidate sent request-ack (slow-vs-dead)
  ackTimer: ReturnType<typeof setTimeout> | null  // ACK_TIMEOUT_MS for this send
  deadlineTimer: ReturnType<typeof setTimeout>    // overall resource deadline
  promise: Promise<Settled>   // resolves only after finalization/persistence
  // deliver(frame): synchronous claim-once (guarded by a `delivered` flag) that
  // hands the raw response (or null on timeout/exhausted failover) to the owner
  // routine, which then finalizes asynchronously.
  deliver: (frame: ResponseFrame | null) => void
  // onAck(g, sock): the matching request-ack arrived — no-op unless (g, sock)
  // still match the current attempt; on match set acked=true, cancel the ack
  // timer, stop failing over.
  onAck: (g: number, sock: WebSocket) => void
  // failover(g, sock): ack timeout, send-throw, or candidate close — no-op unless
  // (g, sock) still match the current attempt (discards stale callbacks); on match
  // deprioritize the current epoch, advance to the next candidate within
  // MAX_FAILOVER/deadline (bumping generation), or deliver(null).
  failover: (g: number, sock: WebSocket) => void
}
private inflight = new Map<'dashboard' | 'manifest', Inflight>()
```

The attempt limiter is **not** in this in-memory map: it lives in DO storage
under `nextAllowedAt:<resource>` (see backpressure). `MIN_REFRESH_MS = 60_000`.
The in-memory `inflight` map is only for in-process coalescing and is safe with
hibernatable WebSockets: an in-flight device `fetch` awaits the promise, keeping
the DO active, so the matching `webSocketMessage` (or `webSocketClose`) lands on
the same live instance.

**`DashboardRelay.fetch`** (device GET, after `authorizeDevice`). The in-flight
`promise` resolves to an **already-finalized result** — `{ kind:'fresh', body }`
or `{ kind:'fallback' }` — only **after** any persistence has completed. The
entry's owning routine is the *only* code that validates the response and writes
the snapshot; waiters never store. Order matters — **coalesce first, limiter
second, become-owner last** — so a fetch arriving while the owner is still
building joins the in-flight build instead of being turned away by the cooldown
the owner just armed:

1. `resource = url.pathname.endsWith('/ota/manifest') ? 'manifest' : 'dashboard'`.
2. **Coalesce (first)**: if `inflight.has(resource)`, `result = await
   entry.promise` (joins the live build, regardless of the cooldown). Skip to
   step 6.
3. **Attempt limiter (only when no in-flight)**: read `nextAllowedAt:<resource>`
   from storage. If `Date.now() < nextAllowedAt`, do not emit: dashboard →
   `result` = stored snapshot tagged with `stale` from its own `lastPublishAt`
   age; manifest → `result` = `fallback` (`404`). Skip to step 6.
4. **Build candidate list**: read the persisted `deprioritized[resource]` set (and
   **prune** epochs no longer among the current sockets), then `candidates` =
   `getWebSockets()` filtered to `OPEN` sockets whose stored `caps` include
   **`resource`** (legacy/no-`hello`/not-`resource`-capable excluded), sorted by
   **(epoch ∉ deprioritized[resource] first, then highest-epoch first)**. Empty →
   `result = { kind:'fallback' }`, skip to step 6 (nothing emitted, no fan-out,
   cooldown left unarmed — so a Worker-ahead-of-old-API deploy falls back instantly
   instead of waiting the deadline).
5. **Become owner**: `id = randomUUID()`. **Set up all settlement state
   synchronously, before any `await`**, so a `webSocketClose/Error` that fires
   during the cooldown write cannot race ahead of the timers: create the
   `deferred` + `delivered` claim-once flag, set `generation = 0`, `attempt = 0`,
   `acked = false`, `socket = candidates[0]`, **arm the overall
   `deadlineTimer = setTimeout(deadline[resource]) → deliver(null)`**, and register
   the `inflight` entry (so step-2 fetches coalesce immediately).
   `deliver(frame|null)` is the synchronous claim (guarded by `delivered`); it only
   resolves the `deferred`. Then `result = await entry.promise`. The owner routine:
   - `await storage.put('nextAllowedAt:'+resource, Date.now()+MIN_REFRESH_MS)`
     (atomic cooldown arm, **once**; survives hibernation; holds for both
     outcomes regardless of how many failover sends occur).
   - **`sendToCandidate()`** (per attempt): bump `entry.generation++`, capture
     `const g = entry.generation`, `const sock = entry.socket`; set `acked = false`.
     **Only if not `delivered`** and `sock.readyState === OPEN`, send
     `{type:'request', id, resource}` on `sock` and arm
     `entry.ackTimer = setTimeout(ACK_TIMEOUT_MS) → entry.failover(g, sock)`. On a
     send throw (or `sock` not OPEN), call `entry.failover(g, sock)` directly.
   - **`entry.failover(g, sock)` — synchronous claim *before* any await
     (round-9/round-10).** The guard and the transition claim are **fully
     synchronous**, completed before the first `await`, so two callbacks (e.g. a
     simultaneous ACK timeout and `webSocketClose`) cannot both pass:
     1. **Synchronous guard:** return immediately if `delivered`, or
        `g !== entry.generation`, or `sock !== entry.socket` (discards a stale
        callback — e.g. an `ackTimer` that fired after a close already advanced).
     2. **Synchronous claim:** still in the same tick, **`entry.generation++`**
        (invalidating any other queued callback for the old `g`) and clear the live
        `ackTimer`. Decide the transition synchronously: if
        `attempt + 1 < min(candidates.length, MAX_FAILOVER)` and the deadline has
        not passed, `attempt++`, `socket = candidates[attempt]` (capture the old
        `sock`'s epoch for the next step); else mark "exhausted".
     3. **Then (async) persist + act:** `await` adding `epochOf(sock)` to
        `deprioritized[resource]`; afterwards, if a next candidate was claimed call
        `sendToCandidate()` (it bumps `generation` again for the new send), else
        `deliver(null)`. Because the generation was bumped **synchronously in step
        2 before this await**, a concurrently queued callback for the old attempt
        already fails its step-1 guard and cannot double-advance, regardless of the
        storage await's latency.
   - **`entry.onAck(g, sock)`**: guarded the same way (`g === entry.generation &&
     sock === entry.socket && !delivered`); on match set `acked = true`, clear
     `ackTimer` (`null`), and stop failing over — `request-ack` proved the current
     candidate is alive; from here only the `deadlineTimer` and the eventual
     `response` matter. (A deadline expiry *after* ack still deprioritizes the epoch
     in finalize — the ack-then-hang case.)
   - `const frame = await deferred` (resolved by the validated matching `response`,
     the overall deadline, or exhausted failover).
   - **Finalize (async).** Let `ep = epochOf(entry.socket)`. First classify `frame`:
     - `frame === null` (deadline / exhausted failover): if `acked` (ack-then-hang)
       add `ep` to `deprioritized[resource]`; `resolve {kind:'fallback'}`.
     - `frame` is a `response` — **validate exactly one of `payload` / `error` is
       present** (neither, or both → treat as malformed = error path):
       - **`error` (or malformed)**: response-phase failure for this resource — add
         `ep` to `deprioritized[resource]`; `resolve {kind:'fallback'}`.
       - **`payload`, dashboard**: this is a **solicited** response from the
         selected candidate, so store **unconditionally** (no preferred-socket gate)
         and clear the flag atomically: `try { await storeDashboard(payload);
         clearDeprioritized('dashboard', ep); resolve {kind:'fresh', body:
         {...payload, stale:false}} } catch { log; resolve {kind:'fallback'} }`
         (`storeDashboard` still dedups by `updatedAt` — see below — for the
         `publishCount` invariant).
       - **`payload`, manifest**: `clearDeprioritized('manifest', ep)`;
         `resolve {kind:'fresh', body: payload}` (no snapshot persistence).
     A successful payload response thus **clears** that `(resource, epoch)` flag;
     a `null`/`error`/malformed outcome on an acked candidate **sets** it. Clearing
     is per-resource: a healthy dashboard response never clears a manifest flag.
   - `finally`: **`clearTimeout(deadlineTimer)` and `clearTimeout(ackTimer)`
     unconditionally** (both always cleared so no timer keeps the DO awake) and
     `inflight.delete(resource)` — deleted only after the promise has settled, so
     waiters never resolve before persistence, never resolve twice, and never hang
     on a rejection.
6. **Every** fetch (owner and waiters) then `recordFetch()` (so `fetchCount`
   counts device GETs) and builds its HTTP response from `result`: `fresh` →
   `result.body`; `fallback` → `fallback(resource)`; a stored-snapshot `result`
   (step 3) → that snapshot.
7. `fallback('dashboard')` = stored snapshot with `stale: true`, else `404`.
   `fallback('manifest')` = always `404` (`{ error: 'Manifest unavailable' }`).

**Response headers + edge-cache invariant.** Every device-facing response
(dashboard and manifest, including `404`/error and fallback paths) sets
`Cache-Control: no-store`. But `no-store` alone is **not** sufficient as a
security guarantee: a Cloudflare Edge Cache TTL / Cache Rule with
`override_origin` explicitly ignores origin cache-control
(https://developers.cloudflare.com/cache/how-to/cache-rules/settings/). Because
the manifest's no-replay property is a security invariant, the deployment must
**also** carry an explicit Cache Rule (or Worker route config) that **bypasses
cache** for `/d/*/ota/manifest` — preferably all authenticated `/d/*` routes.
This requirement is documented in the new ADR + README operating notes and
asserted in tests at the level we control (the Worker sets `no-store` on every
`/d/*` response, including manifest `404`); the Cache Rule itself is a
deploy-time invariant called out in the docs. `no-store` is retained as
defense-in-depth.

**Two store paths — solicited response vs unsolicited push (round-11) — plus
`updatedAt` dedup.** Storing the fallback snapshot must keep it authoritative
without freezing it. The fix splits the two sources rather than using a monotonic
high-water-mark epoch (which deadlocked both recovery paths — round-11):
- **Solicited response (owner routine).** A validated on-demand `response` comes
  from the socket the relay *selected* for the current dashboard request — it is
  authoritative **by construction**. The owner therefore **stores it
  unconditionally** (no epoch gate) and **atomically clears that epoch's
  `deprioritized['dashboard']` flag**, so an only-remaining, previously
  deprioritized publisher self-heals the moment it actually answers (the
  deadlock where the gate rejected the very response that should clear the flag is
  gone).
- **Unsolicited periodic push (`dashboard` handler).** A push is accepted **only
  from the socket that is *currently* the preferred dashboard candidate** —
  computed live as `selectCandidates('dashboard')[0]` (highest-epoch,
  dashboard-capable, **not** in `deprioritized['dashboard']`), compared by socket
  identity. This is **dynamic**, not a monotonic `snapshotEpoch`: when a newer epoch
  is deprioritized (or its socket is gone), the **healthy older** publisher becomes
  the preferred one and its push is accepted, so the snapshot is **never
  permanently frozen** while live requests succeed; a stale lower-epoch retained
  socket pushing while a healthier candidate exists is **not** preferred and is
  ignored. (No `snapshotEpoch` high-water-mark is persisted.)
- **`updatedAt` dedup (both paths).** The on-demand response finalize and a
  periodic push can carry the **same** API build (the API reuses one shared build —
  see the coordinator), so a naive `publishCount += 1` in each path would
  double-count one build. `storeDashboard` stores + increments `publishCount` +
  updates `lastPublishAt` **only when `payload.updatedAt !== storedUpdatedAt`**; an
  equal `updatedAt` is a no-op. The on-demand owner never increments `publishCount`
  itself — `storeDashboard` is the single authority. `updatedAt` is the existing
  per-build ISO timestamp, so two frames from one build are counted once.

Net effect for N concurrent dashboard fetches that trigger one build **against a
single healthy publisher**: `publishCount += 1` (once, via the dedup),
`fetchCount += N`, and that publisher receives exactly one `request` frame even if
a periodic push of the same build races it.

**Honest cross-publisher build bound (no false "one build" guarantee).** The
single-build guarantee is **per publisher** (each API process coalesces its own
builds — see the API coordinator). It is **not** global across distinct
publishers: in the dead-socket ack-failover path (layer 1), `candidates[0]` can
receive the `request` and *start building* while its `request-ack` is delayed or
lost; after `ACK_TIMEOUT_MS` the relay sends the **same `id`** to
`candidates[1]`, which also builds. So a single device refresh can trigger up to
**`MAX_FAILOVER` concurrent builds across distinct publisher processes**. This is
bounded (≤ `MAX_FAILOVER`), only occurs when **multiple** publishers are
connected (normal operation has one → exactly one build), and the per-resource
cooldown still bounds *owner requests* (not these cross-publisher executions). The
relay accepts only the **first** matching `response` (claim-once) and discards
later ones, and `storeDashboard`'s `updatedAt` dedup prevents double counting; the
extra build's fan-out is bounded by each publisher's own adapter caches +
coordinator. The plan and tests budget for ≤ `MAX_FAILOVER` builds per refresh
rather than claiming exactly one.

**Per-resource deadlines**, derived **bottom-up from the slowest adapter's
*composite* worst case** (round-10), not picked top-down, so a normal-but-slow
build (notably the Claude 401→refresh→retry path) completes fresh instead of
spuriously falling back:

```
Claude adapter composite = getAccessToken(lock + optional pre-probe refresh)
                           + probe + (on 401) refresh + probe   (all sequential)
  CLAUDE_ADAPTER_BUDGET_MS (11 000)   // single end-to-end deadline; every step
                                      // bounded by min(step cap, deadline − now)
  < API DASHBOARD_BUILD_BUDGET_MS (12 000)   // ≥ slowest adapter composite; `error` if exceeded
    < RESPONSE_DEADLINE_MS.dashboard (16 000) // = ACK + BUILD + HOP_MARGIN (round-12)
      < firmware relay-profile timeout (22 000)    // relay profiles only; = deadline + net margin
        < firmware fetch-cycle budget (60 000)     // caps total awake-for-fetch
manifest: build is instant (no I/O)
  < RESPONSE_DEADLINE_MS.manifest (6 000)    // = ACK + HOP_MARGIN (+ ~0 build)
    < firmware OTA_MANIFEST_TIMEOUT_MS (10 000)

failover / margin (within each resource deadline, not additive to it):
  ACK_TIMEOUT_MS (2 000)        // per-send liveness probe; MAX_FAILOVER (2) sends max
  RELAY_HOP_MARGIN_MS (2 000)   // round-12: ack send + response serialize/send +
                                // DO receive + finalize/storage + event-loop slack
// cross-request deprioritization is a persisted PER-RESOURCE epoch set cleared
// only by a successful on-demand response for that resource or reconnect — no
// wall-clock TTL, and a periodic dashboard push never clears it
```

**Relay-deadline margin (round-12).** The dashboard deadline must cover the worst
**successful** path — one dead-socket ack hop (`ACK_TIMEOUT_MS`) **then** a full
build on the candidate that answers (`DASHBOARD_BUILD_BUDGET_MS`) — **plus** the
work the earlier draft omitted: the answering publisher's `request-ack`, the
`response` serialize + WS send, and the DO's receipt + finalize (storage write) +
event-loop scheduling. A bare `ACK + BUILD = 14 000` left a build completing near
its 12 000 budget losing the race after one failover. So
`RESPONSE_DEADLINE_MS.dashboard = ACK_TIMEOUT_MS + DASHBOARD_BUILD_BUDGET_MS +
RELAY_HOP_MARGIN_MS = 2 000 + 12 000 + 2 000 = 16 000`, and the firmware
relay-profile timeout is rederived as `16 000 + ~6 000` network margin (DNS/TLS,
edge, DO cold start, transfer) `= 22 000` (still well under the 60 000 fetch-cycle
budget). Manifest, whose build is instant, needs only `ACK_TIMEOUT_MS +
RELAY_HOP_MARGIN_MS ≈ 6 000` with firmware `OTA_MANIFEST_TIMEOUT_MS = 10 000`. A
test drives a failover where the **second** build resolves close to its maximum
budget and asserts the response still beats the relay deadline.

**Claude composite budget (round-10 + round-11).** The Claude adapter's worst case
is **not** a single 10 s refresh, and it is **not** only the probe/refresh HTTP
calls. The full sequential path the adapter can walk in one build is:
**`getAccessToken()`** — which does filesystem reads, a `proper-lockfile`
acquisition (today `retries: 10` with backoff), and **may refresh an expired token
*before* the first probe** — then the **usage probe**, and on a `401` an OAuth
**refresh** (re-entering the credential store + lock) and a **second probe**.
Independent per-op sub-timeouts that each merely sit *below* the build budget do
**not** bound their *sum*, and the 10-retry lock backoff alone can exceed the
12 s budget under contention (round-11), keeping admission closed past it. So the
adapter owns **one** end-to-end deadline `CLAUDE_ADAPTER_BUDGET_MS = 11_000`
captured at entry (`deadline = start + budget`) and threaded into **every**
blocking step — `getAccessToken`/`refreshAndPersist`, the **lockfile acquisition
(retries capped by `deadline − now`, not a fixed 10)**, `callRefresh`, and each
`probeUsage` fetch — so each is bounded by `min(its cap, deadline − now)` and the
**total** is guaranteed ≤ the composite. The common 401 path (cached/valid creds,
uncontended lock) completes **fresh** within it; pathological lock contention or a
pre-probe-refresh-plus-401 double-refresh degrades to the stored snapshot (stale)
for that one build but **never** exceeds the budget or wedges admission. The API
`DASHBOARD_BUILD_BUDGET_MS (12 000)` is **derived as ≥ that composite** (adapters
run concurrently in `buildDashboardPayload`, so the build budget tracks the
*slowest* adapter, Claude), and the relay/firmware deadlines cascade above it.
Tests exercise (a) the worst-case 401-refresh-retry path completing inside the
budget, (b) **lock contention** capped by the remaining budget (not 10 retries),
and (c) an **expired-token-before-first-probe** refresh folded into the composite.

**Deadline-budget rationale (why one in-request build, not two).** The 16 000 ms
dashboard deadline budgets **one ack-failover hop + one full build + the hop
margin**: worst case `candidates[0]` is dead (no ack, ≈2 000 ms) → resend to
`candidates[1]` which acks and builds up to `DASHBOARD_BUILD_BUDGET_MS` (12 000),
then `RELAY_HOP_MARGIN_MS` (2 000) covers the ack/response serialize+send + DO
finalize → 2 000 + 12 000 + 2 000 = 16 000. It deliberately does **not** budget
two *full* builds: an ACK-then-hang on `candidates[0]` would need ≈12 000 to detect
plus another ≈12 000 to rebuild on `candidates[1]` ≫ the relay-profile firmware
timeout (22 000). That case is therefore handled **across requests** (deprioritize
the bad epoch → the next poll picks the healthy publisher first), not by a second
in-request build. The API build budget bounds the response time
regardless of any single adapter (including an unbounded one like GitHub), so the
relay deadline is only a backstop for a *silent/dead* publisher; the firmware
relay-profile timeout sits above it with margin for DNS/TLS, Worker + DO cold
start, storage reads, and transfer. The owner routine clears both its deadline and
ack timers in `finally` so no timer keeps the DO alive. (Constants carry an
asserted-ordering comment in code.)

**`webSocketMessage`**:

- `hello` → parse `capabilities: string[]` (the resources this socket serves on
  demand, e.g. `['dashboard','manifest']`); merge into the socket's stored
  attachment via `ws.serializeAttachment({ epoch, caps })` (preserving the epoch
  set at accept). This is what promotes a socket from legacy to capable. A
  malformed/oversized `capabilities` is ignored (socket stays legacy).
- `ping` → `pong` (unchanged).
- `dashboard` → **accept only if `ws` is the currently preferred dashboard
  candidate** (`selectCandidates('dashboard')[0] === ws`); if so `storeDashboard(payload)`
  (dedups by `updatedAt`), else **ignore** the push (a lower-epoch/deprioritized or
  retired retained socket cannot overwrite the authoritative snapshot — round-11).
  Always reply `ack`. The push does **not** clear any deprioritization flag
  (round-10: a push proves only dashboard-emit liveness, not request/response or
  manifest health — see deprioritization).
- `request-ack` → parse `{ id }`. Since the ack carries only `id`, scan the
  in-flight map values (≤2 entries) for one whose `id === frame.id` **and
  `entry.socket === ws`** (current candidate). Match → `entry.onAck(entry.generation,
  ws)`. A late ack from a *superseded* candidate fails the `entry.socket === ws`
  check and is ignored.
- `response` → parse `{ id, resource }`. **Look up `inflight.get(resource)`
  first** and ignore the frame entirely (no store, no counter) unless the entry
  exists, its `id` matches, **and `entry.socket === ws`** (the response must
  arrive on the **current candidate** socket the request was last sent on — this
  rejects a late response replayed on a superseded candidate or a replacement
  socket after reconnect). Only then call `entry.deliver(frame)` (the finalize step
  validates `payload` xor `error`, maps `error`/malformed to the response-phase
  failure path, and otherwise stores the fresh payload). `deliver` is claim-once,
  so a second response for the same `id` is a no-op.
- else → `{ type:'error', error:'invalid_frame' }`.

**`webSocketClose(ws)` / `webSocketError(ws)`**: for every `inflight` entry whose
**current candidate** `socket === ws`, call `entry.failover(entry.generation, ws)`
(the generation+socket guard makes this a no-op if the attempt already advanced —
e.g. a simultaneous ack-timeout — so close and timeout cannot double-advance). On
a match it deprioritizes `epochOf(ws)` for the entry's resource, advances to the
next R-capable candidate within `MAX_FAILOVER`/the deadline, or `deliver(null)` →
`fallback` when exhausted. A disconnect mid-request therefore retries the next
healthy publisher
at once, or returns last-known data (or `404`) immediately when none remains,
instead of waiting the full deadline. `acceptPublisher` does **not** touch
in-flight entries — it never closes or replaces an existing socket (never-kick); a
request follows its own candidate list, re-derived only at owner setup.

**Stats**: `lastPublishAt`, `publishCount`, `lastFetchAt`, `fetchCount` as
today; a fresh on-demand dashboard response counts as a publish via
`storeDashboard` — but only **once per build** because of the `updatedAt` dedup,
so an on-demand response and a periodic push of the same build do not
double-count.

### API — `api/src/relay/relay-client.ts`

1. Add `getManifest?: () => Promise<OtaManifest> | OtaManifest` to
   `RelayPublisherOptions`.
2. **Process-wide dashboard-build coordinator (single in-flight build, internal
   budget + abort).** The single in-flight build lives in a **process-wide
   coordinator** `createDashboardCoordinator(adapters, tz)` exposing
   `getDashboard(signal?): Promise<DashboardPayload>` (replacing the old boolean
   `publishing` guard), constructed **once** in `api/src/index.ts` and shared by
   **every** build path — the direct Fastify `/dashboard` route, the relay seed,
   the relay periodic push, and on-demand `request` handling. Because all callers
   funnel through it, the single-build guarantee holds **process-wide**, not just
   across relay callers (the previous wiring let the direct route build
   independently — round-8). The coordinator owns the deadline and the abort so no
   single caller can poison or starve the others; cancellation needs **two
   separate** mechanisms — `abort()` alone cannot reject a non-cooperative promise,
   and a race alone leaves the underlying work running:
   - **Admission is keyed on the *underlying* build settling, not on the guarded
     race (round-10).** Track the underlying `build` promise and the `sharedGuarded`
     race **separately**. On the first call when idle, create **one internal
     `AbortController`** — the **only** signal ever passed to
     `buildDashboardPayload(now, adapters, tz, { signal: internal })` — set
     `currentBuild = build`, and `sharedGuarded = Promise.race([build,
     rejectAfter(DASHBOARD_BUILD_BUDGET_MS = 12_000)])`. The race settles every
     awaiter at the budget even if `build` never resolves; the deadline branch calls
     `internal.abort()`. **Admission (`currentBuild`) is cleared only when the
     underlying `build` actually settles.** While `currentBuild` is still unsettled
     (an orphan whose abort the providers ignored), a new `getDashboard` call does
     **not** start a second build; it returns the existing `sharedGuarded` (already
     rejected at the budget → callers **fail fast**). This holds the true one-build
     invariant: at most **one** underlying build runs process-wide at a time, so
     repeated direct `/dashboard` calls (which the relay's 60 s limiter does **not**
     bound) cannot stack orphan builds every 12 s. Normal case: cooperative abort
     terminates the build (the internal signal fires through the adapters), `build`
     settles promptly, admission reopens, and the next call builds fresh.
   - **Settlement plumbing — no unhandled rejection, *all* timers cancelled
     (round-11/round-12).** Carry a per-build `settled` flag (set in `onSettle`) and
     hold the build's `budgetTimer` **and** its `graceTimer` (below) on the build
     record. Attach cleanup via `void build.then(onSettle, onSettle)` (**not**
     `build.finally(...)`, whose returned promise rejects when `build` rejects and
     would itself need catching — round-11). `onSettle` is non-throwing and runs:
     `settled = true`; **`clearTimeout(budgetTimer)` and `clearTimeout(graceTimer)`**
     (so neither a late budget fire nor the grace callback can act after the build
     settled — round-12); and `if (currentBuild === build) { currentBuild = null;
     sharedGuarded = null }`. Because `then(onSettle, onSettle)` consumes **both**
     fulfilment and rejection of `build`, a failed build never raises an unhandled
     rejection (no fragile separate `build.catch`).
   - **Terminal liveness backstop for a non-cooperative build (round-11/round-12).**
     Keeping admission closed until the build settles is correct **only if** the
     build eventually settles; a provider that ignores `abort()` **and** never
     settles would otherwise wedge admission forever. Since all adapters honour the
     internal signal + their own deadlines, this is practically unreachable, but the
     coordinator carries a last-resort backstop: when the budget fires it
     `internal.abort()`s and arms `graceTimer = setTimeout(ABORT_GRACE_MS ≈ 5 000)`
     **bound to this build**. The grace callback is **guarded** — it calls
     `onUnrecoverable()` only if `currentBuild === build && !settled` — and
     `onSettle` clears `graceTimer`, so a build that **cooperatively settles during
     the grace window does NOT terminate a healthy process** (round-12). Only a
     build still unsettled at grace expiry invokes the injectable
     `onUnrecoverable()` hook (default `() => process.exit(1)`), so the process
     **deliberately terminates and the restart supervisor brings up a fresh API**
     (the self-host compose already runs the API under a restart policy — a
     documented deployment prerequisite). Tests inject a spy for `onUnrecoverable`
     and assert it fires on the never-settling path and does **not** fire when the
     build settles within grace.
   - **Per-caller wrapper races the shared build with the caller's own signal
     (never combined into the build).** `getDashboard(callerSignal?)` returns, per
     caller, `Promise.race([sharedGuarded, abortRejection(callerSignal)])` where
     `abortRejection` rejects when `callerSignal` aborts and **removes its
     `abort` listener on settlement** (no listener leak). A caller's `signal`
     therefore settles **only that caller** and is **never** merged into the signal
     the shared build observes (no `AbortSignal.any` into the build) — so one
     caller aborting (e.g. its relay request deadline) can **not** abort or starve
     the build that other callers (or the direct route) still await. The shared
     build is cancelled **only** by the coordinator's own internal budget. (In
     practice the relay per-request deadline ≥ the coordinator budget, so the
     internal budget is normally the binding settlement; the per-caller signal is
     for tests and future tighter per-caller deadlines.)
   - **Production-operation termination (cooperative abort).** The race only
     unblocks awaiters; to actually stop provider work, `buildDashboardPayload`
     forwards the **internal** `AbortSignal` to every adapter, which passes it to
     its `fetch`/child-process and stops on abort. Each adapter must
     **independently** bound its own I/O (Claude via the composite budget above,
     the Codex app-server already kills its child at 8 s; **add a bounded fetch
     timeout to the GitHub adapter**, which has none) so work terminates even if
     one signal observer misbehaves.
   - **Overlap policy.** If a provider ignores `abort()` and keeps running, its
     orphan is bounded by that adapter's own timeout and its result discarded;
     because **admission stays closed until the underlying build settles** (above),
     a new build cannot start on top of an orphan, so at most one build runs at a
     time and orphans cannot accumulate even under unthrottled direct calls.
   - **Public interface changes (so cancellation is actually wired, not just
     raced).** Each signal hop is an explicit signature change:
     - `RelayPublisherOptions.getPayload`: `() => …` → `(signal?: AbortSignal) =>
       Promise<DashboardPayload>` — the signal is **optional** (round-12), wired in
       `api/src/index.ts` to `() => coordinator.getDashboard()`. **No relay call
       path passes a per-caller signal**: on-demand, seed, and periodic all rely on
       the coordinator's internal budget (there is no ad-hoc controller to manage
       across on-demand / seed / periodic / socket-close / stop). The optional
       `signal` parameter exists only for the coordinator's own tests and any future
       per-caller deadline.
     - `createDashboardCoordinator` / `getDashboard(signal?)`: new module
       (`api/src/dashboard-coordinator.ts` or alongside `dashboard.ts`); the
       `signal?` arg is optional and, when omitted, the call is bounded solely by
       the internal budget.
     - `buildDashboardPayload(date, adapters, tz, opts?)`: add `opts?.signal:
       AbortSignal`, forwarded to **every** adapter's `getService`.
     - `DashboardServiceAdapter.getService()` → `getService(signal?: AbortSignal)`
       (the registry interface in `dashboard.ts`). All adapters implement the new
       optional param.
     - `ClaudeCredentialStore.getAccessToken()` → `getAccessToken({ forceRefresh?,
       signal?, deadline? })`, with `refreshAndPersist`, the `proper-lockfile`
       acquisition (retry budget derived from `deadline`), and `callRefresh` all
       taking the deadline/signal (round-11 — the credential path was previously
       unbounded by the adapter budget).
     - Provider propagation points — **every network op AND the credential path get
       the signal + a deadline-derived timeout** (round-9 found the Claude *usage
       probe* unbounded; round-11 found the *credential acquisition* unbounded):
       - **Claude (composite-budgeted, sequential — incl. the credential path).**
         The adapter holds a single end-to-end deadline
         (`deadline = start + CLAUDE_ADAPTER_BUDGET_MS`) and threads **both** the
         forwarded `signal` and `deadline` through the **entire** chain, not just
         the probe HTTP:
         - `getClaudeUsage(signal, deadline)` → **`probeUsage(token, signal,
           deadline)`** for **both** probe calls (`claude.service.ts` does the
           primary request and the post-refresh retry; today *neither* has a
           timeout/signal).
         - **`ClaudeCredentialStore.getAccessToken({ signal, deadline })`** and
           `refreshAndPersist` honour the deadline: the **`proper-lockfile`
           acquisition retries are capped by `deadline − now`** (not the fixed
           `retries: 10` backoff, which alone can exceed the budget under
           contention — round-11), and a **pre-probe refresh** of an expired token
           is part of the composite.
         - `callRefresh` keeps an `AbortController` but its timeout is
           `min(its cap, deadline − now)` and it also observes the forwarded
           `signal`.
         Each fetch's `AbortController` timeout is that `min`, so the whole
         credential-acquire → probe → (401) refresh → probe path fits the composite
         (completes fresh in the common case) yet a build-budget abort still tears
         down any live filesystem/lock/Anthropic work — no orphaned probe and no
         lock held past the budget.
       - **Codex**: the adapter aborts/kills its child process on `signal` (it
         already self-kills at 8 s).
       - **GitHub**: has **no** timeout today — gains both a bounded fetch timeout
         **and** `signal` forwarding (combine via `AbortSignal.any` or an internal
         controller).
       Each network op keeps its **independent** timeout (below the coordinator
       budget) so a misbehaving signal observer cannot hang the build, and the
       internal-signal abort actually terminates in-flight provider work rather than
       leaving orphaned requests.
     - **Direct route change:** the direct `/dashboard` route stops calling
       `buildDashboardPayload` itself and instead calls
       `coordinator.getDashboard()` (no per-caller signal needed), so it shares the
       one in-flight build and the budget. This is the behavioural change that
       makes the process-wide guarantee real.
3. **Handle inbound `request` frames**. Inside `connect()`, **capture the
   connection's socket** (`const socket = ws`) and attach the message handler to
   `socket`, using `socket` throughout — not the outer mutable `ws`, which a
   reconnect reassigns (an old handler must answer on its own socket, never on
   the replacement). Parse JSON; if `type === 'request'`, **first send
   `{ type:'request-ack', id }` synchronously on `socket`** (before awaiting the
   build) so the relay's ACK timer sees this publisher is alive and does not fail
   over to another socket for a merely-slow build; then dispatch by `resource`:
   - `dashboard` → `payload = await coordinator.getDashboard()` — **no per-caller
     signal** (round-12): the relay request handler relies on the coordinator's
     **internal** budget/abort (item 2), which is the binding bound; there is no
     ad-hoc controller to define here. If it rejects (budget/abort or a failed
     build), send an `error` response so the relay falls back immediately; a later
     request builds fresh once admission reopens. The budget (12 000) is derived to
     sit above the slowest adapter's **composite** worst case (the Claude
     credential→probe→refresh→probe path ≈11 000) so a routine token refresh still
     completes fresh.
   - `manifest` → `payload = await options.getManifest()` (fresh build; no shared
     promise and no budget — it does no external I/O). The client only advertised
     `'manifest'` when `getManifest` is wired, so a manifest `request` cannot reach
     a manifest-less publisher; defensively, if `getManifest` is somehow undefined,
     send an `error` response rather than an undefined payload.
   - Each in its own `try/catch`; re-check `socket.readyState === OPEN` after the
     await; send on `socket` `{ type:'response', id, resource, payload }`, or on
     build error `{ type:'response', id, resource, error }` so the relay falls
     back immediately rather than waiting the deadline. If `socket` is no longer
     OPEN, drop the response (do not send on any other socket). Log failures.
4. **Reconnect behavior unchanged.** The relay never closes publishers (newest
   epoch wins; see relay section), so the client keeps its existing
   reconnect-with-backoff on **every** close — no special close codes, no
   terminal stop. This is why two auto-reconnecting publishers converge (the
   relay simply routes to the newest-epoch socket) without oscillation and
   without ever leaving the legitimate publisher permanently offline.
5. **On `open`**: **first send `{ type:'hello', capabilities }` on `socket`** —
   synchronously, before the seed — where `capabilities` lists the resources this
   publisher actually serves: `['dashboard', ...(options.getManifest ? ['manifest']
   : [])]`. So a build without `getManifest` advertises only `'dashboard'` and is
   never asked for a manifest (round-10). The relay routes a resource's `request`
   only to a socket advertising that resource. (An older API without this code
   never sends `hello`, so the relay keeps it legacy and falls back instead of
   waiting the deadline — the backward-compat path.) Then seed the
   dashboard fallback once via
   `publishCurrentDashboard(socket)`. No manifest seed (manifest is never
   cached). Keep the periodic interval as fallback-refresh + WS keepalive
   (unchanged default; no longer the device's freshness source).
   `publishCurrentDashboard` must take the **captured `socket`** (not the outer
   `ws`) and re-check `socket.readyState === OPEN` after the awaited build before
   sending — the seed, the periodic timer, and on-demand handling all await the
   shared build, and a reconnect during that await must not send an old
   connection's result on the replacement socket, send twice, or throw on a null
   `ws`. The periodic timer is per-connection (armed in `connect()` with its
   `socket`, cleared on that socket's close).
6. Clear timers on stop (existing).

### API — wiring (`api/src/index.ts`)

- **Instantiate the adapter registry once** and build **one coordinator** over
  it: `const adapters = createDashboardAdapters()`; `const coordinator =
  createDashboardCoordinator(adapters, tz)` at startup. Today the wiring calls
  `createDashboardAdapters()` **inside** `getPayload`, so every build gets fresh
  adapters and discards the GitHub adapter's 60 s cache (and any Anthropic/Codex
  memoization) — letting a relay build every `MIN_REFRESH_MS` force a full
  provider fan-out. One shared registry means back-to-back builds hit those
  caches.
- **Wire both build paths through the coordinator** so the single-build guarantee
  is process-wide:
  - relay: `getPayload: () => coordinator.getDashboard()` (no per-caller signal —
    round-12);
  - direct `/dashboard` route handler: `await coordinator.getDashboard()` instead
    of its own `buildDashboardPayload(...)` call.
- Pass `getManifest: () => getOtaManifest()` into `createRelayPublisher`.

### Firmware — HTTP timeout margins (transport-specific + bounded fetch cycle)

The relay round-trip needs a per-attempt timeout **above** the 16 000 ms relay
deadline, but raising the timeout *globally* is unsafe: `fetch_one` runs inside
`api_client_fetch_with_failover`, which loops up to `DASHBOARD_HTTP_ATTEMPTS` (3)
× up to `MAX_APIS_PER_NETWORK` (5) profiles. A blanket 22 000 ms would let a
network-level hang keep the device awake ≈15 × 22 s ≈ 5 min and would penalise
**direct** profiles that need no such margin (round-8). Two changes bound this:

- **Transport-derived per-attempt timeout (direct profiles unchanged).** Pick the
  HTTP timeout per profile from the URL shape rather than one global constant: a
  **relay** profile (device route — path begins `/d/`) uses
  `RELAY_HTTP_TIMEOUT_MS = 22000` (relay dashboard deadline 16000 ⇒ ~6 s slack for
  DNS/TLS/edge/DO cold start/transfer); a **direct** LAN/HTTPS profile keeps
  `DASHBOARD_HTTP_TIMEOUT_MS = 10000` (**unchanged** — direct profiles are not
  penalised). The classification is a pure helper
  `bool api_url_is_relay(const char *url)` (host-agnostic: matches the `/d/<id>`
  device path), unit-tested on host.
- **Total fetch-cycle budget.** Add `DASHBOARD_FETCH_CYCLE_BUDGET_MS = 60000`,
  enforced in `api_client_fetch_with_failover`: record a monotonic start
  (`esp_timer_get_time`) and, **before each `fetch_one` and each retry/failover
  delay**, if elapsed ≥ budget, stop the loop, set `out->offline = true`, and
  return. This caps the whole multi-profile/multi-attempt cycle at ≈budget + one
  in-flight timeout (≈82 s worst case) regardless of profile count or per-attempt
  timeout, instead of ≈5 min — and applies to direct profiles too. The budget is
  comfortably above one full relay attempt (22 s) so a single slow relay fetch
  still completes; it only cuts off pathological multi-profile hangs.
- `firmware/main/ota_client.c`: `OTA_MANIFEST_TIMEOUT_MS` 6000 → **10000** (relay
  manifest deadline 6000 ⇒ ~4 s slack). The manifest is fetched on a **single**
  profile (not the failover loop), so it is **not** multiplied by attempts/
  profiles and needs no cycle budget of its own.

These extend the worst-case awake time only on a hung relay fetch and are now
**bounded**; the relay fallback normally returns well inside the deadline. The
cumulative worst case is documented and covered by a host test on
`api_url_is_relay` + the cycle-budget arithmetic.

### Firmware — `firmware/main/display.c` (decouple `stale` from `offline`)

1. Line ~1670 `bool offline = data->offline || data->stale;` →
   `bool offline = data->offline;`. `stale` no longer hijacks the header: a
   genuine `data->offline` still shows the `OFFLINE` header; a stale (relay
   fallback) frame shows the normal `DEVDASH` header with the Wi-Fi/API slot
   indicators and the sync-time cluster.
2. Lines ~1917-1924: only `data->offline` marks `DISPLAY_FRAME_OFFLINE_API` and
   suppresses `s_last_content_valid`. A stale-but-online frame sets
   `s_last_content_valid` and marks `DISPLAY_FRAME_CONTENT` like fresh content,
   so the cheap partial-update / unchanged-skip paths stay enabled (no more
   per-cycle full refresh).
3. **Stale indicator — black plane only.** When `data->stale && !data->offline`,
   draw a small marker in the **black** framebuffer near the sync cluster (e.g.
   a filled dot left of the `[sync]` glyph). It must not touch the red plane:
   reusing the red `icon_cross_sync()` would put red in the framebuffer while
   `dashboard_needs_physical_red()` / `s_last_red_state` report none (stale is
   excluded from `offline` and from `alert_requested`), desyncing BWR refresh
   bookkeeping. A black-only marker keeps `need_red` consistent and preserves BW
   partial-update eligibility. The frozen sync timestamp already hints at age;
   the marker makes "last-known, not live" explicit on both BW and BWR panels.

### Firmware — `firmware/main/main.c` (set the wall clock from fresh data only)

`main.c:324-326` calls `timekeep_set_from_iso(data.updated_at_iso)` after every
successful fetch — **including a stale relay fallback**. A stale snapshot carries
an old `updatedAtLocalIso`, so during an API outage every wake would rewind the
RTC to that frozen timestamp; quiet-hours time would advance by one sleep
interval and then be reset again, never progressing. Gate the clock-set on a
fresh payload: `if (data.updated_at_iso[0] && !data.stale)`. The RTC then
free-runs across sleeps during an outage (acceptable drift for quiet hours)
instead of being pinned. Extract the decision into a pure predicate
`bool clock_should_apply(const char *iso, bool stale)` so it can be host-tested
(the RTC/main orchestration itself is not host-testable); the end-to-end
quiet-hours progression across repeated stale fetches stays a manual on-device
check.

### Firmware — already implemented (build/test only)

`ota_version.{c,h}`, `ota_client.{c,h}`, `firmware/main/CMakeLists.txt`, and the
host test `firmware/test/` (project + `main/` component + `test_ota_version.c` +
`README.md`) exist and pass (16/16 on the IDF `linux` target). The main firmware
builds clean with the upgrade-only + canonical-URL changes.

### Docs

1. New ADR `docs/decisions/0007-relay-on-demand-transport.md`: push→on-demand;
   why a proxy is impossible (NAT/no ingress; only the API dials out); the
   request/response correlation over the existing WS; the **`hello`/capability
   negotiation** (resource-specific: a publisher advertises the resources it serves,
   so a manifest-less build is never asked for a manifest) and why it is required
   for a safe Worker-then-API rollout (a legacy publisher is never sent `request`,
   so old firmware falls back inside its shorter timeout instead of hanging);
   never-kick publisher model (strictly-increasing storage epoch, newest capable
   OPEN socket) with the **two-layer failover**: in-request ACK failover for a dead
   socket (`MAX_FAILOVER`, generation+socket-guarded with a **synchronous claim
   before any await** so concurrent close/timeout callbacks cannot double-advance)
   plus cross-request **persisted, PER-RESOURCE, evidence-cleared deprioritization**
   for an ack-then-hang/error socket — cleared **only** by a successful on-demand
   response *for that resource* or reconnect-with-new-epoch, **never by wall-clock
   TTL** (round-9) and **never by a periodic dashboard push** (round-10: a push
   proves only dashboard-emit liveness, not request/manifest health); correctness
   routing comes from the in-request layer every request, so the persisted set only
   reorders; the **fallback-cache store model** — a solicited on-demand response is
   stored unconditionally (and clears its epoch's flag), while an unsolicited
   periodic push is accepted only from the *currently preferred* dashboard candidate
   (dynamic, **not** a monotonic `snapshotEpoch` — round-11 — so a healthy older
   publisher can replace a deprioritized newer one's snapshot and an only publisher
   self-heals); why kicking/terminal-close was rejected (oscillation / persistent
   DoS); the **deadline budget** rationale (one ack-hop + one build fits; a second
   full build would exceed the firmware timeout, so ack-then-hang recovers across
   requests) **derived bottom-up from the Claude adapter's sequential composite
   budget** — which now spans the **credential path too** (`getAccessToken` lock
   acquisition with retries capped by the remaining deadline + optional pre-probe
   refresh) + probe + refresh + probe (round-10/round-11), not picked top-down; the
   **process-wide build coordinator** shared by the direct route and the relay — one
   internal abort signal drives the shared build, per-caller wrappers race it with
   their own signal **without** ever feeding a caller signal into the shared build,
   **admission stays closed until the underlying build settles** so a non-cooperative
   orphan can never be stacked on, cleanup via `then(onSettle,onSettle)` (no
   unhandled rejection), and a **terminal liveness backstop** (`onUnrecoverable` →
   process exit after an abort grace, relying on the deployment restart supervisor)
   for a build that ignores abort and never settles (round-11) (the single-build
   guarantee is intra-process; cross-publisher failover can start ≤ `MAX_FAILOVER`
   builds — stated honestly); per-resource deadlines with
   margin and fallback (dashboard snapshot vs manifest `404`); coalescing +
   persistent attempt limiter (backpressure); the **transport-specific firmware
   timeout + total fetch-cycle budget** (relay profiles get the longer timeout,
   direct profiles unchanged, and the whole multi-profile cycle is bounded — not
   ~5 min); the manifest no-cache
   replay rationale **including the required Cloudflare cache-bypass Cache Rule for
   `/d/*` (over-and-above `no-store`)**; and the **honest OTA security limit** (the
   publish key selects any eligible official firmware *path*, not merely a `404`;
   URL pinning constrains the path only — there is **no firmware-verified
   signature**, so binary authenticity rests on TLS + GitHub repo access control;
   key rotation as the compromise response; an embedded-key manifest/digest
   signature as the future option for true binary authenticity).
2. Rewrite the OTA-over-relay section of `docs/decisions/0005-ota-updates.md`:
   manifest fetched on demand over the relay (no TTL/heartbeat, no cache);
   firmware pins the canonical URL and is upgrade-only; `404` is a graceful skip;
   state plainly that URL pinning constrains the path, not the bytes (no signature
   verification today).
3. `README.md`: relay diagram, route/profile table, operating notes — relay
   profiles now deliver live dashboard + on-demand OTA; document that
   `RELAY_PUBLISH_KEY` can select **any eligible newer** official release (not just
   cause a `404`), that the canonical-URL pin + upgrade-only bound *which path* but
   provide **no binary-authenticity signature**, and the key-rotation compromise
   response. **Deployment prerequisite:** the API must run under a **restart
   supervisor** (the self-host compose already sets a restart policy) — the
   coordinator's terminal liveness backstop deliberately exits the process if a
   build ignores abort and never settles, relying on the supervisor to bring up a
   fresh API.

## Tests

### Relay — `relay/test/relay.test.ts`

- **On-demand dashboard**: connect a publisher that answers
  `{type:'request',resource:'dashboard'}` with a `response`; `GET /d/<uuid>`
  returns that fresh payload with `stale: false`.
- **On-demand manifest**: same for `resource:'manifest'` on
  `GET /d/<uuid>/ota/manifest`.
- **Manifest never cached**: after a successful manifest fetch, disconnect the
  publisher; the next manifest fetch returns `404` (no fallback), proving no
  replayable manifest cache.
- **Dashboard fallback**: no publisher + a stored snapshot → `stale: true`;
  nothing stored → `404`.
- **Coalescing**: fire N concurrent `GET /d/<uuid>` while the publisher answers
  once; assert the publisher received **one** `request` frame, all N responses
  match, and counters move by `publishCount += 1` / `fetchCount += N`.
- **Min-refresh rate limit (sustained sequential traffic)**: issue M rapid
  *sequential* dashboard fetches inside `MIN_REFRESH_MS`; assert the publisher
  received only **one** `request` frame and the later fetches returned the
  cached fresh snapshot (`stale:false`); the same sequential burst for the
  manifest yields one build then `404`s within the window. (Use a short
  test-injected `MIN_REFRESH_MS`.) The API-side counterpart (in
  `relay-client.test.ts`) asserts sustained sequential `request` frames produce a
  **bounded** number of provider/`getPayload` calls (reused adapters +
  coalescing), not one per request.
- **Timeout fallback through the full Worker route**: publisher connected but
  silent; the device GET (routed through `worker.fetch`) returns the dashboard
  snapshot with `stale: true` (or `404`) after the deadline, and the manifest GET
  returns `404`. Use a short test-injected deadline (env/binding) so the test
  does not wait 9 s.
- **Error response → immediate fallback**: publisher replies
  `{type:'response',id,resource,error}`; relay falls back without the deadline.
- **Publisher selection / lifecycle**: with two coexisting capable publisher
  sockets the relay sends the `request` only to the higher-epoch OPEN socket (the
  older one receives none — assert no duplicate fan-out); after the newest socket
  closes mid-request the relay fails over to the next-highest capable OPEN socket
  (same `id`) and the response on that socket is accepted; the relay never closes
  a publisher itself.
- **Capability gating / mixed-version rollout**: (a) a publisher that connects but
  never sends `hello` (legacy) is **never** sent a `request` — a device fetch with
  only a legacy publisher connected falls back **immediately** (dashboard snapshot
  / manifest `404`), it does **not** wait the deadline; (b) with one legacy and one
  `hello:['dashboard','manifest']` publisher connected, the `request` goes only to
  the capable one; (c) after the capable publisher sends `hello`, requests route to
  it. Asserts the Worker-ahead-of-old-API window degrades to fast fallback, not a
  hang.
- **Resource-specific capability (round-10)**: a publisher advertising
  `hello:['dashboard']` only — a manifest `GET` is **never** routed to it (it never
  receives a manifest `request`, gets no undefined payload, and is **not**
  deprioritized for dashboard); the manifest fetch falls back to `404`. A separate
  publisher advertising `['dashboard','manifest']` receives both. Proves a
  manifest-less build cannot poison dashboard delivery.
- **ACK-based same-request failover (dead socket)**: (a) the highest-epoch
  capable socket receives the `request` but never sends `request-ack`; after
  `ACK_TIMEOUT_MS` (test-injected short) the relay resends the **same `id`** to the
  next candidate, which acks + responds → the fetch returns that fresh payload
  (assert exactly the expected number of `request` frames, ≤ `MAX_FAILOVER`); (b)
  the first candidate **does** send `request-ack` then stays silent past
  `ACK_TIMEOUT_MS` → the relay does **not** fail over (slow-but-healthy), and the
  eventual `response` on that same socket is accepted; (c) all candidates fail to
  ack → fallback after `MAX_FAILOVER` sends, never exceeding the resource deadline.
  The cooldown is armed once regardless of failover count.
- **Cross-publisher build bound (no false one-build claim)**: with two capable
  publishers, candidate[0] receives the `request` and begins building but its
  `request-ack` is dropped; after `ACK_TIMEOUT_MS` candidate[1] also receives the
  same `id` and builds → assert **both** publishers built (≤ `MAX_FAILOVER`
  builds), only the **first** matching `response` is accepted (claim-once) and
  `publishCount` moves by exactly 1 (dedup), proving the documented bound rather
  than an impossible "exactly one build."
- **Response-phase failure → per-resource, evidence-cleared deprioritization (no
  permanent blackhole, no wall-clock re-promotion, no cross-resource/push rehab)**:
  candidate[0] (highest epoch) **acks** then never responds (or replies `error`)
  for `dashboard`; the fetch falls back and the relay adds `epoch0` to
  `deprioritized['dashboard']`. On the **next** dashboard request, candidate[1]
  (healthy, older) is selected **first** and answers fresh.
  - **Repeated-poll (round-9 regression)**: run **many** sequential polls with
    candidate[0] still ack-then-hang and **advance the clock arbitrarily far** —
    assert epoch0 is **never** re-promoted by elapsed time and candidate[1] answers
    **every** poll (no every-other-poll fallback).
  - **Per-resource isolation (round-10)**: epoch0 deprioritized for `dashboard` is
    still a first-class candidate for `manifest` (the flags are independent); and a
    publisher broken on `manifest` (acks then errors manifest) that **keeps sending
    successful periodic `dashboard` pushes** stays deprioritized for `manifest`
    across many polls — assert the push does **not** clear the manifest flag and
    manifest keeps routing to a healthy publisher / `404`, never recurring-fallback
    via push rehab.
  - **Clearing only on real evidence**: the `(resource,epoch)` flag clears on (a) a
    later successful on-demand `response` **for that resource** from epoch0, or (b)
    epoch0's socket closing and reconnecting with a **new higher** un-flagged epoch
    — **not** on a periodic dashboard push. Also: the sets survive a simulated
    hibernation (state reload); flags for a closed socket's epoch are pruned; a
    deprioritized publisher that is the **only** R-capable candidate is still
    selected and self-heals on its next successful response for R.
- **Preferred-socket push gate + both recovery paths (round-10/round-11)**:
  - **Rolling deploy:** a newer (higher-epoch) publisher is preferred; an **older
    retained** socket emits a periodic `dashboard` push → ignored (no snapshot
    overwrite, `publishCount` unchanged). A push from the *preferred* (highest-epoch
    non-deprioritized) publisher with a new `updatedAt` is accepted and increments
    once.
  - **Older publisher replaces newer snapshot (round-11):** after the newer epoch
    is **deprioritized for dashboard** (or its socket closes), the **older healthy**
    publisher becomes preferred → its push **is accepted** and replaces the snapshot
    (proves the snapshot is not permanently frozen behind a monotonic high-water
    epoch).
  - **Only, previously-deprioritized publisher self-heals (round-11):** with a
    single publisher whose epoch is in `deprioritized['dashboard']`, it is still
    selected (only candidate); its successful on-demand **response** is stored
    **unconditionally** and **clears** its flag (proves the store path does not
    reject the very response that should rehabilitate it).
- **Failover idempotency under concurrent callbacks (round-9)**: drive a
  **simultaneous** ack-timeout + `webSocketClose` (and a separate send-error +
  `webSocketClose`) for the same attempt; assert `failover(g, sock)` advances the
  attempt **exactly once** (generation+socket guard) — the stale second callback is
  a no-op, so the request does not skip a candidate or exhaust `MAX_FAILOVER`
  prematurely, and `deprioritized[resource]` gains the epoch once.
- **Error/malformed response finalization (round-9 MINOR)**: a matched `response`
  carrying `error` (and one carrying **neither** `payload` nor `error`, and one
  carrying **both**) each map to the response-phase failure path — fallback served,
  the acked epoch deprioritized, no dashboard stored, `publishCount` unchanged — and
  are **not** mistaken for a fresh payload.
- **Response correlation hardening**: a `response` with a wrong `id`, a wrong
  `resource`, one arriving on a **different socket** than the request was sent on
  (cross-socket), and one arriving **after the deadline** each must be ignored —
  assert none stores a dashboard, moves `publishCount`, or settles a newer
  in-flight request. Conversely, a matching response on the request's **original**
  socket **stays valid even after a newer publisher has connected** (never-kick).
- **Close during cooldown write**: close the selected socket while the owner's
  `storage.put(nextAllowedAt)` is still pending; the fetch settles to fallback at
  once and no deadline timer is left live (the timer is armed before the await
  and cleared in `finally`).
- **`storeDashboard` updatedAt dedup**: an on-demand `response` and a periodic
  `dashboard` push carrying the **same** `updatedAt` increment `publishCount` only
  **once** (the second store is a no-op); a push with a **new** `updatedAt`
  increments again. Proves the overlap invariant from the design.
- **Response headers**: dashboard, manifest, and manifest-`404` responses all
  carry `Cache-Control: no-store`.
- Auth unchanged: `401` wrong device token; `400` invalid uuid; spoofed internal
  headers rejected (existing tests stay green).

### API — `api/src/relay/relay-client.test.ts`

- **Hello on open (resource-specific)**: the first frame the client sends after
  `open` is `{ type:'hello', capabilities:['dashboard','manifest'] }` (or
  `['dashboard']` when no `getManifest` is wired), before the dashboard seed.
- **Request-ack before build**: on receiving a `request`, the client sends
  `{ type:'request-ack', id }` **synchronously** (before the awaited build
  resolves) — assert the ack frame is observed prior to the `response`, including
  when the build is slow.
- A `request` frame for `dashboard` triggers the build and a `response`; same
  for `manifest` → `getManifest`.
- Concurrent `dashboard` requests share one build (`coordinator.getDashboard`):
  `getPayload` called once.
- **No per-caller signal on any relay path (round-12)**: assert the on-demand
  handler, the seed, and the periodic timer each call `coordinator.getDashboard()`
  with **no** argument (the optional `signal` is undefined), so all relay build
  paths are bounded solely by the coordinator's internal budget — no ad-hoc
  controller to leak across on-demand/seed/periodic/close/stop.
- A **failed** dashboard build clears the shared slot and a subsequent request
  **succeeds** (proves the rejected promise is not retained/poisoning).
- `getPayload`/`getManifest` rejection → an `error` response, no unhandled
  rejection.
- Socket closing between the `await` and `send` is handled (readyState recheck).
- **Reconnect convergence**: every close → reconnect with backoff (no terminal
  stop, no special close code). With the relay's newest-epoch routing this
  proves two auto-reconnecting publishers converge without oscillation and that
  a disconnected legitimate publisher always comes back (no permanent-offline).
- A response is sent only on the socket that received the `request`, never on a
  reconnected replacement socket (the handler captured its own `socket`).
- **Reconnect during a periodic/seed build**: when `ws` is reassigned while a
  `publishCurrentDashboard` build is awaiting, the result is sent on the original
  captured socket (or dropped if it closed), never on the replacement, and never
  throws.
- **Slot settlement (race) + orphan admission (round-10)**: an injected
  `getPayload` that **never resolves** → the budget race rejects the awaiter at
  `DASHBOARD_BUILD_BUDGET_MS` and an `error` response is sent, but **admission
  stays closed** while the orphan runs: assert that subsequent on-demand/direct
  calls during the orphan **fail fast** (return the already-rejected guarded
  promise) and do **not** start a second `buildDashboardPayload`. Only after the
  underlying build settles (resolve the injected promise, or it aborts) does
  admission reopen and a later request build fresh. Proves no orphan accumulation
  even under unthrottled repeated calls.
- **Operation termination (abort) — incl. the Claude usage probe (round-9)**: a
  build passed the internal `AbortSignal` → on budget the signal fires and **each**
  network op observes it: assert `probeUsage`'s `fetch` is called **with the
  signal** and rejects on abort (both the primary and the post-refresh retry path),
  `callRefresh` honours it, the GitHub adapter's fetch is called with the signal,
  and the Codex child is killed. Proves no Anthropic/GitHub request is left live
  after a timed-out build (the gap round-9 found: the probe had no timeout/signal).
- **Adapter registry reused**: `createDashboardAdapters()` is constructed once;
  N builds reuse it (assert the adapter factory is not re-invoked per build), so
  provider-cache windows apply across builds.
- Seed-on-open sends one dashboard frame and no manifest frame.
- Timer cleared on stop.

### API — `api/src/dashboard-coordinator.test.ts` (process-wide single build)

- **Direct route + relay share one build**: a direct `/dashboard` call and a relay
  on-demand `request` that overlap in time resolve from **one**
  `buildDashboardPayload` invocation (assert build called once), proving the
  guarantee holds across *both* entry points, not just relay callers.
- **Budget bounds the direct route; admission gated on underlying settle
  (round-10)**: a `buildDashboardPayload` that **settles late** (after the budget)
  → a direct `getDashboard()` caller settles (rejects) at
  `DASHBOARD_BUILD_BUDGET_MS` and the internal signal fires, but `currentBuild` is
  **not** cleared until the underlying build actually settles — assert that
  repeated direct calls during the orphan **do not** spawn new builds (admission
  closed, callers fail fast) and exactly **one** build is ever in flight; once it
  settles, admission reopens and a later call builds fresh.
- **Settlement plumbing (round-11)**: a build that **rejects** does not produce an
  `unhandledRejection` (cleanup attached via `then(onSettle, onSettle)`, not
  `finally`), the `rejectAfter` budget timer is cleared on settle (assert no late
  fire / no leaked timer), and `currentBuild`/`sharedGuarded` reset exactly once.
- **Terminal liveness backstop (round-11)**: inject a `buildDashboardPayload` that
  **never settles** and a spy `onUnrecoverable`; after the budget abort + an
  injected-short `ABORT_GRACE_MS`, assert `onUnrecoverable` is invoked **once**
  (the process-restart path) — and assert it is **not** invoked when the build
  settles within the grace window. Proves a truly non-cooperative build cannot
  permanently wedge admission.
- **Claude composite incl. credential path (round-10/round-11)**: (a) the
  401→refresh→retry path completes **within** the budget and returns fresh; (b)
  **lock contention** — `proper-lockfile` cannot acquire — is bounded by the
  remaining adapter deadline (not the fixed 10-retry backoff) and the build still
  settles within `DASHBOARD_BUILD_BUDGET_MS`; (c) an **expired token before the
  first probe** triggers a pre-probe refresh that is folded into the composite and
  still fits; a path exceeding the composite aborts cleanly (each sub-op + the lock
  retry bounded by `min(cap, deadline − now)`).
- **External caller signal does not abort co-awaiters or the shared build
  (round-9)**: two callers await the same build, one passing a `callerSignal`;
  aborting that signal **rejects only that caller's** wrapper while the shared
  build keeps running and the other caller still resolves with the payload — assert
  `buildDashboardPayload` was **never** passed the caller signal (only the internal
  one) and is **not** aborted by the peer. Also assert the caller wrapper **removes
  its abort listener** on settlement (no listener leak across many calls).

### API — `api/src/routes/ota.test.ts`

- Route serves via `getOtaManifest`. Malformed, overflowing, suffixed, or
  unprefixed (`0.4.0`) `APP_VERSION` → `{ otaEnabled: false }`; canonical
  `v0.4.0` → enabled manifest.

### Firmware host helpers — `firmware/test/` (IDF `linux`, Unity) — passing

`ota_download_url_is_canonical` (reject other repo/asset/version/non-https,
accept canonical, reject NULL) and `ota_version_is_newer` (strictly newer;
reject equal/older/suffixed-equal-base; `v0.9.0 < v0.10.0`; fail closed on
malformed/unprefixed/overflow; tolerant `v` only for `running`). Add
`clock_should_apply(iso, stale)`: true only for a non-empty ISO with
`stale == false`; false for a stale payload or empty ISO (guards the
quiet-hours-rewind regression). Add `api_url_is_relay(url)`: true for a device
relay route (path begins `/d/<id>`, e.g. `https://relay.example/d/abc/dashboard`),
false for a direct LAN/HTTPS API URL (`http://192.168.x/dashboard`,
`https://api.example/dashboard`) and for NULL/empty — so the transport-specific
HTTP timeout selection is unit-covered. The fetch-cycle budget itself is asserted
by arithmetic + a documented worst-case bound (the loop wiring around
`esp_timer_get_time` is exercised on-device, not host-mockable).

## Verification

All firmware commands run inside the devcontainer as `node` (per AGENTS.md):

- Relay: `docker exec -u node optimistic_hermann bash -c "cd /workspaces/eink-devdash/relay && npm test"`
- API: `docker exec -u node optimistic_hermann bash -c "cd /workspaces/eink-devdash/api && npm test"`
- API build: `docker exec -u node optimistic_hermann bash -c "cd /workspaces/eink-devdash/api && npm run build"`
- Firmware build: `docker exec -u node optimistic_hermann bash -c "source /etc/profile.d/esp-idf.sh && cd /workspaces/eink-devdash/firmware && idf.py build"`
- Firmware host tests: build + run the `firmware/test` `linux` target; assert
  the runner exit code is 0.
- Manual: a relay profile shows live data with `stale: false` and a full header;
  pulling the API offline shows last-known dashboard data with the stale marker
  (no per-cycle full refresh) and OTA `404` (skipped); OTA upgrades on a newer
  canonical release and refuses a non-canonical `downloadUrl`.

## Risk / rollback

- The relay round-trip adds latency per fetch; per-resource deadlines + snapshot
  fallback (dashboard) / `404` (manifest), with firmware timeouts raised for
  margin, bound the worst case below the firmware HTTP timeout — a slow/absent
  API degrades to last-known dashboard data and skipped OTA, never a firmware
  fetch failure.
- DO in-memory correlation relies on the in-flight `fetch` keeping the instance
  alive; bounded by the deadline and settled by close/error handlers.
- All wire changes are additive (new frame types) **and capability-negotiated**,
  so rollout order is safe in both directions. An older API never sends `hello`,
  so the relay marks it legacy and never sends it a `request`: device fetches
  fall back **immediately** to the stored dashboard snapshot (with `stale`) or a
  manifest `404`, well inside the older firmware's shorter HTTP timeout — there is
  no 14 s hang waiting for a `response` the old API will never send. Once the API
  is upgraded it sends `hello` on (re)connect and on-demand engages with no relay
  redeploy. Rollback is reverting the relay/api/firmware/doc commits; a reverted
  (old) API simply stops sending `hello` and the relay reverts to fallback
  delivery on the next reconnect.
