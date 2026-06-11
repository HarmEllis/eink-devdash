# Plan: Codex overage spend (`$`) metric

Date: 2026-06-10
Branch: `feat/codex-overage-spend-metric`

## Background

The dashboard's `$` extra-usage bar is a **dormant feature**. The firmware
render path is complete, but the API never produces a value, so the bar shows
on no provider on real hardware.

- **Firmware** (`firmware/main/api_client.c:272-289`, `display.c:754-768`):
  reads a metric with `id == "spend"` from a service's `metrics[]` array
  (`service_metric_present` / `service_metric_value` → integer `value`) and
  renders the `$` bar **only when present**. `value > 0` → red `+N` text and a
  bar filling `min(100, value)` %; `value == 0` → grey `0`.
- **API** (`api/src/services/usage.adapters.ts`, `codex.service.ts`): never
  emits a `spend` metric. `codex.service.ts:513` already notes spend/budget
  mapping as a *future api-key adapter*.

On ChatGPT-auth (Joost's setup) the Codex rate-limit feed exposes only
`usedPercent`, never a dollar figure, so the spend value must be
operator-supplied via env until/unless a real api-key cost source exists.

## Goal

Light up the Codex `$` bar end-to-end by having the API emit a `spend` metric
driven by a new env var, gated so it is **hidden by default** and appears only
when there is real overage spend.

Both providers: the same env-driven `spend` metric now applies to Claude
(`CLAUDE_OVERAGE_USD`) and Codex (`CODEX_OVERAGE_USD`); the producer is shared
via the `spendMetrics()` helper in `usage.adapters.ts`. No firmware changes.

Follow-up (separate plan): replace the manual env with a live read of each
provider's actual extra-usage / credit balance so no value has to be set by
hand. See `docs/plans/2026-06-11-live-usage-credits.md`.

## Wire contract (already supported by the type + firmware)

`DashboardService.metrics?: DashboardMetric[]`, where
`DashboardMetric = { id, label, value, unit?, limit?, resetInSeconds? }`.
Emit, on the Codex service only:

```jsonc
"metrics": [{ "id": "spend", "label": "$", "value": 5, "unit": "USD" }]
```

## Changes

1. **`api/src/services/codex.service.ts`**
   - Add `spend?: number | null` to the local `CodexUsage` type.
   - Add env parse + module const:
     ```ts
     function parseOverageUsd(raw: string | undefined): number | null {
       if (!raw) return null
       const n = Number.parseFloat(raw.trim())
       return Number.isFinite(n) && n > 0 ? n : null
     }
     const CODEX_OVERAGE_USD = parseOverageUsd(process.env.CODEX_OVERAGE_USD)
     ```
   - In the public `getCodexUsage` wrapper, attach it once for every path:
     `return { ...usage, spend: CODEX_OVERAGE_USD }`.

2. **`api/src/services/usage.adapters.ts`**
   - Add `spend?: number | null` to the local `CodexUsage` type.
   - In `serviceFromCodexUsage`, build the service object, then conditionally
     attach metrics:
     ```ts
     if (usage.spend != null && usage.spend > 0) {
       service.metrics = [{ id: 'spend', label: '$',
                            value: Math.round(usage.spend), unit: 'USD' }]
     }
     ```
   - Gate on `> 0` so the bar is truly hidden by default. (Deliberately
     diverges from the README mockup's grey `0` bar; matches Joost's stated
     "hidden until there is overage" intent.)

3. **Tests** — `api/src/services/usage.adapters.test.ts`
   - `serviceFromCodexUsage` with `spend > 0` → `metrics` contains
     `{ id: 'spend', value: <rounded> }`.
   - With `spend` null/undefined/0 → no `metrics`.

4. **Docs / config**
   - `.env.example`: add `CODEX_OVERAGE_USD=` under the Codex block with a
     one-line comment.
   - `docker-compose.yml`: add `- CODEX_OVERAGE_USD=${CODEX_OVERAGE_USD:-}` to
     both service env blocks.
   - `README.md`: add a row to the env-var table.

5. **Mockup (optional, keeps README representative)**
   - Set the Codex mock `spend` to a small positive value (e.g. `3`) in
     `scripts/render-readme-screens.mjs` so the regenerated dashboard SVG shows
     the `+N` overage bar (demonstrates the shipped behaviour). Regenerate with
     the script's run command. Claude mock stays `null` (no bar), illustrating
     "appears only on overage".

## Display quirk to document

Firmware treats `value` as both the USD amount (text `+N`, capped at 999) and
the bar percentage (`min(100, value)`). So `$5` → `+5` and a 5% bar; `$120` →
`+120` and a full bar. Acceptable for a coarse overage indicator; documented in
the env comment.

## Verification

- `npm test` / `npm run build` (or repo equivalent) in `api/` — green.
- Manual: set `CODEX_OVERAGE_USD=5`, hit the dashboard endpoint, confirm the
  Codex service JSON contains the `spend` metric; unset → metric absent.
- Firmware needs no rebuild (contract unchanged).

## Workflow note

Implemented WITHOUT pre-review at Joost's explicit request (atypical), then
reviewed via co-dev claude afterward.
