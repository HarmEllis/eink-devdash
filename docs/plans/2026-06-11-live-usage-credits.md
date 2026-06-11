# Plan: Live usage-credits extra-usage bar (currency-aware)

Date: 2026-06-11
Status: proposal — rev 2 (after Codex review iteration 1 + user design decision)
Supersedes the env-only spend from `2026-06-10-codex-overage-spend-metric.md`.

## Goal

Drive the Claude extra-usage row from the **real** credit data, no hand-set env.
Render it as a currency-aware row:

```
€  [######----]  0.91
↑      ↑            ↑
sym    bar =        amount spent
       utilization  (used_credits, major units)
       (used/limit)
```

- **symbol** ← `extra_usage.currency` (`€` for EUR, `$` for USD, …)
- **bar** ← `extra_usage.utilization` (a true 0-100 gauge: share of the monthly
  credit cap consumed), decoupled from the amount
- **amount** ← `extra_usage.used_credits` in major units (e.g. `0.91`)

Codex keeps the manual `CODEX_OVERAGE_USD`: ChatGPT-auth exposes only an optional
*remaining-credit* snapshot (`credits:{has_credits,balance}`), no used-spend or
monthly-limit — verified, see "Codex source" below.

## Verified source (Claude, 2026-06-11)

`GET https://api.anthropic.com/api/oauth/usage` with the Claude Code OAuth bearer
+ `anthropic-beta: oauth-2025-04-20` → HTTP 200:

```jsonc
{
  "five_hour": { "utilization": 33.0, "resets_at": "2026-06-11T09:40:00Z" },
  "seven_day": { "utilization": 21.0, "resets_at": "2026-06-11T10:00:00Z" },
  "extra_usage": {
    "is_enabled": true, "monthly_limit": 1700, "used_credits": 91.0,
    "utilization": 5.35, "currency": "EUR", "disabled_reason": null
  }
}
```

`monthly_limit`/`used_credits` are **minor units** (1700 = €17.00, 91 = €0.91).
Same backend the Claude Code UI uses (OAuth app slug `claude-code`) — undocumented,
so treated as best-effort with a fallback. It also yields the 5h/7d utilization
that `claude.service.ts` currently obtains via a **billed** `POST /v1/messages`
probe; the GET lets us stop spending usage just to read limits.

## Codex source (verified — keep env)

Sandbox blocked the live `wham/usage` HTTP probe (DNS), but local Codex 0.139.0
session data shows the rate-limit schema carries `credits:{has_credits:false,
unlimited:false, balance:"0"}` (mostly `null`) — a remaining-credit snapshot only,
no spend-used / utilization / monthly-limit. The only verified read surface is the
app-server `account/rateLimits/read` JSON-RPC (mirrored by `token_count.rate_limits`).
→ No live dollar source for ChatGPT-auth Codex. Keep `CODEX_OVERAGE_USD`.

## Wire changes (`api/src/services/dashboard-service.ts`)

`DashboardMetric` already has `value`, `unit?`, `limit?`. Add one field:

```ts
export type DashboardMetric = {
  id: string; label: string; value: number
  unit?: string          // currency code, e.g. "EUR"
  usedPercent?: number    // NEW: 0..100, drives the bar (≠ amount)
  limit?: number          // monthly cap, major units
  resetInSeconds?: number
}
```

Spend metric emitted (Claude only):
`{ id:'spend', label:'$', value: usedMajor, unit: currencyCode,
   usedPercent: round(utilization), limit: monthlyMajor }`

`value` may be fractional (`0.91`). `label` stays an id hint; the rendered symbol
comes from `unit`. Schema version (`DASHBOARD_SCHEMA_VERSION=2`) stays — additive
field, and the spend metric has never shipped with real data, so no old-firmware
misrender risk in the field; firmware + API ship together here.

## API changes

### `claude.service.ts`
- `type ExtraUsage` + `spend` fields on `ClaudeUsage`:
  `spendAmount?: number|null` (major), `spendPercent?: number|null`,
  `spendLimit?: number|null`, `spendCurrency?: string|null`.
- `fetchOAuthUsage(token, signal, deadline)`:
  - GET `/api/oauth/usage`; parse `five_hour`/`seven_day`
    (`utilization` 0..100 → `{used:round(util), limit:100, resetInSeconds}` from
    `resets_at - now`, clamped ≥0).
  - `extra_usage` → only when `is_enabled === true && used_credits > 0`:
    `spendAmount = minorToMajor(used_credits, currency)`,
    `spendPercent = clamp(round(utilization),0,100)`,
    `spendLimit = minorToMajor(monthly_limit, currency)`,
    `spendCurrency = currency`. Else all `null` (→ no metric).
  - Guard every field: non-finite/negative/missing → treat as absent, never throw.
- `minorToMajor(minor, currency)`: divide by `10 ** exponent(currency)`.
  `exponent`: small map (EUR/USD/GBP/CHF…=2, JPY/KRW=0, BHD/KWD=3); unknown →
  default 2 **and `console.warn`**. (Addresses Codex's "÷100 assumes 2 decimals".)
- **Precedence** (Codex): `CLAUDE_OVERAGE_USD` override, if set (>0), wins and
  forces `spendAmount`=env, `spendCurrency`='USD', `spendPercent`/`spendLimit`
  from env only if derivable (else percent omitted → bar falls back to amount-capped).
  Otherwise live value. Otherwise absent. Documented in `.env.example`.
- **Fallback state machine** in `getClaudeUsage` (Codex):
  - GET 200 with ≥1 present window → use GET result (windows + spend); **no probe**.
  - GET 200 but **no** usable windows → fall through to probe (window data only).
  - Disabled/missing/zero `extra_usage` on an otherwise-good GET → windows from GET,
    **no spend metric, and do NOT trigger the probe** (probe is window-only).
  - GET 401 → invalidate cache, force refresh, retry GET once; still 401 → probe path.
  - GET 403/429/5xx/timeout/malformed/partial JSON → probe fallback (preserves
    today's auth-error semantics).
  - Parent `signal` abort → propagate (throw), never swallow.

### `usage.adapters.ts`
- Carry the four spend fields on the local `ClaudeUsage`.
- Replace `spendMetrics(spend)` with `spendMetrics(s: {amount,percent,limit,currency})`:
  emit `{id:'spend', label:'$', value: s.amount, unit: s.currency,
  usedPercent: s.percent, limit: s.limit}` only when `amount != null && amount > 0`.
  Codex still has no live source → its adapter passes amount from `CODEX_OVERAGE_USD`
  only (percent/limit/currency omitted → firmware uses the legacy amount-as-percent
  bar for Codex, unchanged).

## Firmware changes (`firmware/main/display.c` + `api_client.c`)

- `api_client.c`: for the `spend` metric also read `usedPercent` (double),
  `value` (double), `unit` (string) — extend `service_metric_*` helpers /
  `dashboard_data_t` (`spend_amount`, `spend_percent`, `spend_limit`,
  `spend_currency[4]`).
- `display.c` spend block (currently `display.c:754-768`):
  - bar pct = `spend_percent` (fallback to legacy amount-capped only when percent
    absent, for Codex/env).
  - left symbol = currency glyph by code: `$` (existing font), `€` (new),
    `£`/`¥` optional; default to currency-code first letter if unknown.
  - right text = amount with up to 2 decimals (`%.2f`, trim trailing for whole
    numbers), capped at a sane width.
- **`€` glyph**: the `font5x7[][5]` table is the 95-glyph ASCII invariant the
  README render script asserts (`scripts/render-readme-screens.mjs:26`). Do **not**
  grow it. Add a tiny dedicated `draw_currency_symbol(x,y,code,red)` with a 5x7
  bitmap for `€` (and reuse font `$`), so the ASCII table stays 95.
- `scripts/render-readme-screens.mjs`: mirror the new spend layout +
  `drawCurrencySymbol`, and set the Claude mock to a realistic
  `{amount:0.91, percent:5, limit:17, currency:'EUR'}` so the README SVG shows it.

## Config / docs

- `.env.example` / `README.md`: reframe `CLAUDE_OVERAGE_USD` as an **override**
  (USD) that wins over the live read; live read is default. `CODEX_OVERAGE_USD`
  unchanged (no live source).
- `docker-compose.yml`: env entries already present, keep.

## Tests

- `fetchOAuthUsage` parsing: windows from utilization+resets_at; spend from
  extra_usage enabled / disabled / zero / missing.
- `minorToMajor` / exponent: EUR & USD = ÷100, JPY = ÷1, unknown → ÷100 + warn.
- Precedence: env override > live > absent.
- Fallback matrix: 200+windows→no probe; 200 no windows→probe; disabled extra_usage
  →no metric & no probe; 401→refresh→GET retry; 403/429/5xx/timeout/malformed→probe;
  abort propagates.
- Guards: non-finite/negative amount, bad reset timestamp, partial windows.
- Adapter: metric carries value/unit/usedPercent/limit; Codex env path unchanged.

## Validation / done-criteria

- `npm run build` + `npm test` green.
- Manual: live dashboard JSON → Claude windows match `/api/oauth/usage`; spend
  metric present only when extra usage on & `used_credits>0`, with correct
  `value`/`usedPercent`/`unit`. **Confirm the real stats are fetched** before commit.
- README SVG regenerated, shows `€ [bar] 0.91` on Claude.
- Firmware builds (idf.py build) — no flashing required for commit.

## Risks

- Undocumented endpoint: guard all fields, never throw, degrade to the probe.
- Currency exponent: handled in API via known map + warn-on-unknown.
- Firmware `€` glyph: isolated bitmap, keeps the 95-glyph ASCII invariant intact.

## Iteration-2 resolutions (final, blocking issues from Codex)

1. **New metric id `extraUsage`, retire `spend`.** Released schema-2 firmware
   (v0.6.0) reads `spend` as int + `$` + amount-as-bar and does not reject newer
   schema versions, so changing `spend`'s meaning would misrender on un-upgraded
   devices (API and firmware update independently via OTA). Both providers now
   emit `extraUsage`; old firmware simply ignores the unknown id (row absent —
   acceptable for this never-shipped-with-data feature). `spend` id is removed.
   Firmware renders one `extraUsage` block: bar = `usedPercent` when present,
   else amount-capped legacy fallback (Codex env path, which has no percent).

2. **Currency allowlist, no guessing.** Support only `EUR` and `USD` for now
   (both exponent 2; symbols `€` new bitmap / `$` existing font). The API
   converts minor→major with the correct exponent and **omits the metric for any
   other currency** (logged) — no first-letter fallback, no wrong-decimal money.
   Firmware formats the amount with up to 2 decimals, trailing zeros trimmed
   (`0.91`, `1`, `1200`). Extensible later by adding a glyph + allowlist entry.

3. **Fallback, contradiction removed.** After a 200, compute
   `windowsOk = ≥1 of five_hour/seven_day finite` and
   `spendOk = extra_usage enabled & used>0 & currency in allowlist`.
   - `windowsOk` → use GET windows; attach spend iff `spendOk`; **no probe**.
   - `!windowsOk` (incl. partial/malformed-but-parsed) → run the probe for
     windows, and **preserve the GET spend** if `spendOk` (don't discard it).
   - GET non-200 / network / JSON-parse failure → no spend; 401→refresh→retry
     GET once then probe; 403/429/5xx/timeout → probe (windows only).
   "Partial" = body parsed but `!windowsOk`.

(401-refresh and parent-abort propagation were already correct per Codex.)

## Workflow

plan (2 Codex iterations, done) → implement → Codex reviews the work →
if good **and** stats-fetch confirmed → commit.
