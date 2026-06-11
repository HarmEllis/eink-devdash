# Plan: server-side (locale-aware) extra-usage amount formatting

Status: DONE — implemented in commit 544f192 (Codex code-review APPROVE after one
BLOCK fix: unsupported/inherited currencies now omitted at the adapter boundary).
Date: 2026-06-11
Builds on: `2026-06-11-live-usage-credits.md` (live Claude extra-usage already shipped
on branch `feat/codex-overage-spend-metric`, commit 2ac7287, not yet pushed).

## Goal

Move extra-usage amount formatting from the firmware to the API so the decimal
separator follows a real locale instead of the hardcoded "EUR -> comma" rule. The API
emits a preformatted, **ASCII-guaranteed** amount string; the firmware draws it verbatim.

User decision (2026-06-11): server-side via Intl. Timezone is NOT a usable proxy for
number locale, so a dedicated `DASHBOARD_LOCALE` is introduced.

## What moves, and what does NOT

- Moves to API: the decimal separator + the numeric rendering of the amount.
- Stays in firmware: the currency SYMBOL glyph (`€`/`$`). Non-ASCII symbols cannot
  travel in the ASCII font, so the device keeps its own bitmap table keyed on the ISO
  code.
- Stays in API but UNCHANGED in spirit: the **verified** minor-unit exponent allowlist
  (`EUR:2, USD:2`). Per Codex review we do NOT derive the exponent from Intl — ICU/CLDR
  currency metadata is implementation-dependent and cannot prove Anthropic uses ISO
  minor units for an arbitrary currency. Intl is used ONLY for the locale separator and
  latin-digit enforcement, not to decide how many minor units a currency has.

## Design

### 1. Locale source (API)
- New env `DASHBOARD_LOCALE` (BCP-47), default **`nl-NL`** (deterministic,
  backward-compatible with the just-shipped EUR-comma behaviour; consistent with the
  project's `Europe/Amsterdam` default). Never use the runtime/container locale.
- Validate at startup: `Intl.getCanonicalLocales(raw)` for syntax (throws on garbage),
  then `Intl.NumberFormat.supportedLocalesOf([canonical])` for availability (note:
  `new Intl.NumberFormat('xx-YY')` does NOT throw — it silently falls back, so the
  explicit support check is required). On invalid/unsupported: warn once, use `nl-NL`.

### 2. ASCII-safe amount formatting (API, central)
Single helper in a new shared `currency.ts`:
```ts
const SUPPORTED = { EUR: { exponent: 2, symbol: '€' }, USD: { exponent: 2, symbol: '$' } }
const DISPLAY_MAX = 99999   // matches the existing firmware cap

function formatAmount(amountMajor: number, currency: string, locale: string): string {
  const exponent = SUPPORTED[currency].exponent          // verified, NOT from Intl
  // Clamp the DISPLAY value before formatting so the <=15-byte postcondition holds
  // regardless of input (env overrides are arbitrary; 1e15 / MAX_VALUE / NaN / Inf).
  // The numeric `value` on the metric is left UNCHANGED.
  const safe = Number.isFinite(amountMajor)
    ? Math.min(Math.max(amountMajor, 0), DISPLAY_MAX)
    : 0
  const nf = new Intl.NumberFormat(locale, {
    style: 'decimal',
    numberingSystem: 'latn',   // force ASCII 0-9, no Arabic/Devanagari digits
    useGrouping: false,        // no thousands separator (kills NBSP/space group chars)
    minimumFractionDigits: 0,  // trims trailing zeros: 1.20 -> "1,2", 1.00 -> "1"
    maximumFractionDigits: exponent,
  })
  let s = nf.format(safe)
  // Defensive: only digits + at most one ASCII '.'/',' separator, and <=15 bytes.
  if (!/^[0-9]+([.,][0-9]+)?$/.test(s) || s.length > 15) {
    // Should be unreachable post-clamp with latn+no-grouping; dot-format fallback.
    s = safe.toFixed(exponent).replace(/\.?0+$/, '')
  }
  return s
}
```
- The **clamp to `DISPLAY_MAX` is enforced in code**, not just documented, so the
  `<=15` invariant holds for any input (a `1e15` override would otherwise be 16 bytes,
  `Number.MAX_VALUE` ~309 bytes while still matching the digit regex; `NaN`/`Infinity`
  format to `"NaN"`/`"∞"` which the clamp turns into `0`).
- `useGrouping:false` + `numberingSystem:'latn'` means the output is digits plus at most
  one decimal separator, and that separator is ASCII (`.` or `,`) for every locale.
- `minimumFractionDigits:0` does the trailing-zero trimming natively (the previously
  proposed "drop an all-zero fraction" would NOT turn `1,20` into `1,2`).

### 3. Formatting ownership (central, not per-service)
- Generate `valueText` in `usage.adapters.ts` `extraUsageMetric(...)`, the single place
  that builds the metric for all three paths: Claude live, `CLAUDE_OVERAGE_USD`,
  `CODEX_OVERAGE_USD`. All three then share one locale path. It is NOT Claude-service
  presentation state.
- `claude.service.ts` keeps doing only the minor->major conversion + emission gate using
  the same `SUPPORTED` table (moved into `currency.ts` as the single source of truth;
  retires the local `CURRENCY_EXPONENTS`). `usage.adapters.ts` `CURRENCY_SYMBOLS` also
  collapses into `currency.ts`.

### 4. Wire contract
- `DashboardMetric` gains `valueText?: string`, guaranteed **<= 15 ASCII bytes** by the
  API (trivially holds: amount capped, 2 decimals, no grouping). `value` stays numeric —
  still needed for `has_spend` (amount > 0) and the amount-capped fallback bar on the
  override path.
- Additive + optional; released v0.6.0 firmware ignores it (and never emitted a real
  extra-usage metric). **No schema bump** (stays version 2).

### 5. Currency support scope
- Keep the **EUR/USD allowlist** for both the exponent (verified minor units) and the
  symbol. Unknown currency -> metric omitted, exactly as today. Never substitute `$`
  for an unknown currency. Adding a currency later requires: provider verification of
  its minor-unit encoding + a FW symbol glyph + an allowlist entry.

### 6. Firmware
- `extra_usage_t` gains `char value_text[16]` (15 chars + NUL).
- `parse_extra_usage` validates `valueText` before accepting it: it must be <= 15 bytes
  AND match `[0-9]+([.,][0-9]+)?`. Any overlong OR malformed (even short) string is
  REJECTED (leave `value_text` empty) so the device falls back to the numeric path
  instead of showing a wrong/garbled amount.
- `display.c`: if `value_text[0]` is set, draw it verbatim; else fall back to
  `format_spend_amount(value, currency)` (kept for mixed API/FW versions; its EUR-comma
  rule remains only as that fallback). Bar-fit (amount width drives bar width) is
  unchanged and works for any bounded string.

### 7. Render script (`render-readme-screens.mjs`)
- Mock `extra` carries `valueText` (e.g. `"0,91"`); `drawProvider` prefers
  `extra.valueText`, else formats. Regenerate the dashboard SVG.

## Files touched
- new `api/src/services/currency.ts` — `SUPPORTED` table + `formatAmount` + locale
  validation helper (single source of truth).
- `api/src/services/dashboard-service.ts` — `valueText?: string` on `DashboardMetric`.
- `api/src/services/claude.service.ts` — use `currency.ts` for exponent/gate; drop local
  `CURRENCY_EXPONENTS`.
- `api/src/services/usage.adapters.ts` — build `valueText` centrally; use `currency.ts`
  for symbol/label; read `DASHBOARD_LOCALE`.
- `api/src/services/*.test.ts` — formatAmount (nl-NL comma, en-US dot, trailing-zero
  trim, negative/zero clamp, **huge value 1e15 / MAX_VALUE clamped to <=15 bytes**,
  **rounding near the cap**, **NaN / Infinity -> "0"**, ASCII-charset assertion), locale
  validation (canonical + unsupported fallback), valueText on all three metric paths.
- `.env.example`, `README.md`, `docker-compose.yml` (both env blocks) — `DASHBOARD_LOCALE`.
- `firmware/main/api_client.{h,c}` — `value_text[16]` field + bounded/rejecting parse.
- `firmware/main/display.c` — prefer `value_text`.
- `scripts/render-readme-screens.mjs` — `valueText` in mock + draw path.

## Verification
- `cd api && npm run build && npm test` (new formatAmount + locale-validation tests).
- `cd firmware && idf.py build` under the OTA slot.
- Live fetch on the real EUR account yields `valueText: "0,91"` (nl-NL).
- Render the SVG; confirm `0,91` and bar-fit for a wide value (e.g. `12,34`).

## Resolved decisions (from Codex iteration-1)
1. Default `DASHBOARD_LOCALE` = `nl-NL` (deterministic; not runtime).
2. Trim trailing zeros via `minimumFractionDigits: 0`.
3. Keep the EUR/USD allowlist (exponent + symbol); never `$` for unknown currency.
4. ASCII wire contract via `style:'decimal' + numberingSystem:'latn' + useGrouping:false`;
   assert the charset.
5. `valueText` <= 15 ASCII bytes; firmware rejects overlong rather than truncating.
6. No schema bump (additive optional field; numeric fallback retained).
7. (iteration 2) Display value clamped to `DISPLAY_MAX=99999` in code before formatting,
   so the <=15 invariant holds for any input (1e15 / MAX_VALUE / NaN / Infinity); numeric
   `value` unchanged. Firmware rejects malformed short strings too, not only overlong.
