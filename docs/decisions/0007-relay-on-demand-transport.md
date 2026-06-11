# ADR-0007: On-demand transport over the Cloudflare relay

- **Status:** Accepted
- **Date:** 2026-06-07

## Context

The API runs behind home NAT and has no public ingress. Only the API can open
the connection to Cloudflare, so a conventional pull-through reverse proxy
cannot reach it. The original relay periodically pushed a dashboard snapshot.
Device refreshes commonly arrived after that snapshot was considered stale,
which made the display appear offline and forced unnecessary full refreshes.

OTA manifests have a stricter requirement: replaying an old manifest while the
API is offline can select an obsolete but still newer release. A manifest must
therefore never use the dashboard's last-known snapshot behavior.

## Decision

The API keeps its outbound WebSocket and the relay uses it bidirectionally.
A device GET sends a correlated `request` frame to a capable publisher. The API
sends `request-ack` synchronously, builds the resource, then returns a matching
`response`. Dashboard requests may fall back to a stored snapshot; manifest
requests fall back to `404` and are never cached.

The API also sends an application-level heartbeat over the WebSocket. Each
`ping` must receive a `pong` before the configured timeout; otherwise the API
terminates the half-open socket and enters the normal reconnect path. The pong
includes the relay protocol version. A missing or unsupported version keeps the
connection backward compatible but logs a warning that the relay deployment
must be updated.

Publishers send `hello` before their seed dashboard and advertise
resource-specific capabilities. A legacy publisher that never sends `hello` is
never sent a request. This makes a Worker-first rollout fail quickly to the
snapshot or manifest `404` instead of waiting for a response old API code cannot
produce.

### Publisher selection and failover

Each accepted socket receives a strictly increasing epoch persisted in Durable
Object storage. Multiple sockets may coexist and the relay never kicks an older
publisher. For each resource it selects capable, open sockets with healthy
epochs first and higher epochs first.

An unacknowledged request fails over after two seconds, reusing the same request
ID and sending to at most two publishers. Every callback is guarded by both a
generation and socket identity; the failover transition is claimed
synchronously before storage I/O, so a simultaneous close and ACK timeout
cannot advance twice. An acknowledged publisher is allowed to finish its build
and is not failed over merely for being slow.

An ACK followed by timeout, explicit error, or malformed response
deprioritizes that epoch for the affected resource. These sets are persisted,
have no wall-clock expiry, and are cleared only by a successful on-demand
response for that resource or by reconnecting with a new epoch. A periodic
dashboard push does not rehabilitate dashboard request handling or manifest
handling. Deprioritized sockets remain usable when they are the only candidate,
which permits evidence-based self-healing.

A solicited dashboard response is authoritative and is stored unconditionally.
An unsolicited periodic push is stored only from the currently preferred
dashboard socket. Storage deduplicates equal `updatedAt` values so a shared API
build sent through both paths increments publish statistics once.

### Backpressure and deadlines

The relay coalesces concurrent requests per device and resource. Before emitting
an owner request it persists a 60-second per-resource cooldown. The cooldown
applies after success and failure and survives Durable Object hibernation.
Dashboard requests inside the window use the snapshot; manifest requests return
`404`.

The API owns one process-wide dashboard coordinator shared by the direct route,
relay seed, periodic push, and on-demand requests. It races the build against a
12-second budget and aborts every adapter through one internal signal.
Admission stays closed until the underlying build settles, so a provider that
ignores cancellation cannot accumulate overlapping builds. Cleanup uses
`then(onSettle, onSettle)` to consume both outcomes. If work remains unsettled
five seconds after budget abort, the coordinator exits the process through its
terminal liveness backstop.

Production deployments must therefore run the API under a restart supervisor.
The supplied Compose services already have a restart policy.

The 12-second build budget is above the Claude adapter's 11-second composite
deadline, which includes credential lock acquisition, optional pre-probe token
refresh, usage probe, a 401 refresh, and the retry probe. GitHub fetches and the
Codex child process have independent bounds. Relay deadlines are 16 seconds for
dashboard and 6 seconds for manifest. Firmware uses a 22-second timeout only
for `/d/<device>` relay profiles, retains 10 seconds for direct profiles, and
caps the complete dashboard fetch/failover cycle at 60 seconds plus the active
attempt.

The one-build guarantee is process-local. A lost ACK can make at most two
different publisher processes build the same request during failover.

### Cache and OTA security

Every `/d/*` response sets `Cache-Control: no-store` and a unique
`X-Relay-Response-Nonce`. Operators must also configure a Cloudflare Cache Rule
that bypasses cache for `/d/*`, especially `/d/*/ota/manifest`. An Edge Cache
TTL rule that overrides origin headers can otherwise violate the manifest
no-replay invariant. `/admin/cache-bypass-probe` performs two authenticated
manifest requests and reports whether their nonces differ.

`RELAY_PUBLISH_KEY` is a global publisher credential. A holder can select any
canonical official release newer than a device, including a superseded release
that remains newer than that device. Firmware rejects arbitrary hosts, paths,
repositories, assets, downgrades, and malformed versions.

URL pinning constrains the path, not the downloaded bytes. Firmware currently
verifies no release signature or independently trusted digest; authenticity
rests on TLS to GitHub and repository access control. Rotate
`RELAY_PUBLISH_KEY` immediately on suspected compromise. True binary
authenticity requires a future firmware-verified signature over the manifest
and binary digest using an embedded public key.

## Consequences

- Normal relay reads are generated at fetch time.
- Dashboard outages show last-known content with a stale marker, without being
  treated as transport-offline.
- OTA over relay is fresh-only and unavailable manifests are graceful skips.
- Deployments must maintain the WebSocket publisher, restart supervision, and
  Cloudflare cache-bypass rule.
- The protocol remains backward compatible because requests are capability
  negotiated.

## Rejected alternatives

- Public pull-through proxy: impossible without exposing ingress to the home API.
- Periodic manifest heartbeat and TTL: still replayable and unnecessary.
- Closing older publishers: causes reconnect oscillation or persistent denial
  of service after a brief replacement.
- Time-based health promotion: repeatedly restores an ACK-then-hang publisher.
- A second full build after ACK-then-hang: exceeds the firmware timeout budget.
