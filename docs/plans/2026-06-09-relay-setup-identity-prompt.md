# Plan: Prompt to reuse-or-regenerate the relay identity on setup

## Goal

When `relay/scripts/setup.mjs` (run via `npm run setup` or the Docker image)
finds a **complete relay identity already present** in the target `.env`, let
the operator **choose** whether to reuse it or generate a fresh one — instead of
today's silent auto-reuse. This makes the "I want a new identity" path explicit
without forcing the user to hand-edit/delete `.env` first, while keeping the
idempotent reuse behavior the default for non-interactive runs.

## Current behavior (verified)

`main()` in `relay/scripts/setup.mjs:292-308`:

```js
let identity
try {
  identity = identityFromEnv(existingEnv)   // helper already exists + tested
} catch (error) {
  fail(error.message)                        // partial identity → hard fail
}
if (identity) {
  step("Reusing this machine's existing device identity...")
} else {
  step('Generating a fresh device identity...')
  identity = generateIdentity()
}
```

- `identityFromEnv(existing)` (`setup-helpers.mjs`) returns a full identity when
  `DEVICE_UUID` + `DEVICE_TOKEN` + `RELAY_PUBLISH_KEY` are all present
  (upgrading a legacy file by minting a missing `RELAY_ADMIN_KEY`), returns
  `null` when none are present, and **throws** on a partial set.
- So: existing identity → **silent reuse**, no choice offered. That is the gap.
- Docker reads `RELAY_SETUP_ENV_OUT=/out/.env` (the mounted `relay-out/.env`),
  not the repo `.env`; first Docker run sees an empty file → generates new. That
  is why running the container produced an *additional* identity (the Worker
  supports multiple per-UUID secrets since PR #24).

## Design

Add a single decision point with three inputs: an env override (for
non-interactive control), TTY availability, and whether an existing identity was
found. Keep all decision logic pure/testable; keep the actual stdin read thin.

### 1. New pure helper in `setup-helpers.mjs`

```js
/**
 * Decide how to handle this machine's device identity:
 * - 'new'    override=new, OR no existing identity and no reuse override.
 * - 'reuse'  override=reuse (existing required), OR existing + non-interactive
 *            with no override (idempotent default, preserves current behavior).
 * - 'prompt' existing identity, a TTY, and no override — ask the operator.
 * Throws when override=reuse but no complete identity exists, so an explicit
 * reuse against an empty/mis-mounted .env fails loudly instead of silently
 * minting a new identity (Codex review [MAJOR] #2).
 * `override` is the normalized RELAY_SETUP_IDENTITY value ('reuse' | 'new' | undefined).
 */
export function chooseIdentityMode({ hasExisting, override, isInteractive }) {
  if (override === 'reuse') {
    if (!hasExisting) {
      throw new Error(
        'RELAY_SETUP_IDENTITY=reuse, but no complete identity was found in the '
        + 'target .env. Check the file / mount, or unset the override to generate one.',
      )
    }
    return 'reuse'
  }
  if (override === 'new') return 'new'
  if (!hasExisting) return 'new'
  if (isInteractive) return 'prompt'
  return 'reuse'
}
```

Also a small normalizer so an invalid `RELAY_SETUP_IDENTITY` fails loudly rather
than being silently ignored:

```js
export function normalizeIdentityOverride(raw) {
  if (raw == null || raw === '') return undefined
  const v = String(raw).trim().toLowerCase()
  if (v === 'reuse' || v === 'new') return v
  throw new Error(`RELAY_SETUP_IDENTITY must be "reuse" or "new" (got "${raw}").`)
}
```

### 2. Prompt: pure answer-parser + thin readline loop

Per Codex review [MINOR] #3, split the testable part out. A pure helper
normalizes a raw answer; only the readline loop stays untested I/O:

```js
/** 'reuse' | 'new' | null(=unrecognized, re-ask). Bare Enter => 'reuse'. */
export function normalizeReuseAnswer(raw) {
  const v = String(raw ?? '').trim().toLowerCase()
  if (v === '' || v === 'r' || v === 'reuse' || v === 'y' || v === 'yes') return 'reuse'
  if (v === 'n' || v === 'new' || v === 'no') return 'new'
  return null
}
```

The `setup.mjs` loop uses `node:readline/promises` over `process.stdin`/`stdout`,
calls `normalizeReuseAnswer`, and re-asks on `null`. Default on bare Enter =
**reuse** (the safe, non-destructive choice):

```
Existing device identity found (DEVICE_UUID 115a9e87-…).
  [R] reuse it (keep the current device working)
  [n] generate a new identity (the firmware must be re-provisioned)
Choose [R/n]:
```

### 3. Wire into `main()`

```js
const existing = (() => { try { return identityFromEnv(existingEnv) }
                          catch (e) { fail(e.message) } })()
const override = normalizeIdentityOverride(process.env.RELAY_SETUP_IDENTITY) // may fail()
const mode = chooseIdentityMode({
  hasExisting: Boolean(existing),
  override,
  isInteractive: process.stdin.isTTY === true,
})
let identity
if (mode === 'reuse') { identity = existing; step('Reusing this machine\'s existing device identity...') }
else if (mode === 'new') { step('Generating a fresh device identity...'); identity = generateIdentity() }
else { /* prompt */ identity = (await promptReuseOrNew(existing)) ? existing
                              : (step('Generating a fresh device identity...'), generateIdentity()) }
```

Behavior preserved: no existing identity → silent fresh generate; existing +
non-interactive (e.g. `docker run` without `-it`, or CI) → silent reuse.

## Choosing "new" — what actually happens (corrected per Codex [MAJOR] #1)

Choosing **new** mints a fresh UUID, so `pushSecrets` writes a *new* per-UUID
Worker secret (`DEVICE_IDENTITY_<newUUID>`) and `writeEnvFile` overwrites the
`.env` with the new values. The **old** Worker secret is **left in place**, but
that does **not** mean the old device keeps working:

The relay is **on-demand** (commit `39393e5`, "Enforce on-demand relay dashboard
fetches"). The API publisher dials out under exactly **one** UUID — the one in
`.env`. Once setup overwrites `.env` with the new UUID and the publisher
restarts, it connects only under the new UUID. The old UUID still has valid
Worker credentials but **no publisher socket**, so device fetches to the old
`/d/<oldUUID>/…` return `503 … publisher unavailable`.

So the correct framing — for the prompt text and README — is:

- "new" effectively **replaces** the active device from the API's side; the old
  device must be **re-provisioned** with the new URL/token (or you must keep a
  second publisher running under the old UUID).
- The old Worker secret stays **authorized** but unused; **recommend deleting**
  it (`wrangler secret delete DEVICE_IDENTITY_<oldUUID>`) once it's no longer
  needed, rather than implying it keeps anything alive.

## Open question for review

`identityFromEnv` **throws** on a *partial* identity, which today aborts setup.
Two options:
- (A) Keep current hard-fail (minimal; out of scope of this feature).
- (B) When interactive and the file is partial, offer "generate new" instead of
  failing. Friendlier, but changes existing error semantics and needs the
  helper to distinguish "partial" from "valid-but-reuse" for the caller.

Default recommendation: **(A)** for this change; revisit (B) separately.

## Files

- `relay/scripts/setup-helpers.mjs` — `chooseIdentityMode`,
  `normalizeIdentityOverride`, `normalizeReuseAnswer` (all pure).
- `relay/scripts/setup.mjs` — `promptReuseOrNew` (thin readline loop calling
  `normalizeReuseAnswer`) + `main()` wiring + header-comment env-var docs
  (`RELAY_SETUP_IDENTITY`).
- `relay/test/setup-helpers.test.ts` — table tests for `chooseIdentityMode`
  (all input combos **including override=reuse + no existing → throws**),
  `normalizeIdentityOverride` (reuse/new/empty/invalid), and
  `normalizeReuseAnswer` (bare Enter, r/reuse/y, n/new/no, case/whitespace,
  unrecognized → null).
- `README.md` — document `RELAY_SETUP_IDENTITY` and the reuse/new prompt in the
  relay setup section; note `-it` is required for the prompt (else it falls back
  to the non-interactive default), that explicit `reuse` fails if no identity
  exists, and that "new" replaces the active device (old one needs
  re-provisioning; delete the stale Worker secret when done).

## Non-goals

- No deletion/rotation of old Worker secrets.
- No change to `identityFromEnv`'s reuse/legacy-upgrade/partial semantics
  (pending the open question).
- No change to the auth flow, deploy, or secret-push mechanics.

## Test / verify

- `cd relay && npm test` (unit tests for the two new pure helpers).
- Manual matrix: empty `.env` → generates; full `.env` + TTY → prompt, both
  branches; full `.env` + `RELAY_SETUP_IDENTITY=reuse`/`new` (non-interactive) →
  honored; invalid override → clean failure.
