/* Single source of truth for the small set of currencies the dashboard can
 * render. A currency is supported only when we can BOTH convert its minor units
 * correctly (verified exponent — NOT derived from Intl/ICU, whose per-currency
 * metadata is implementation-dependent and cannot prove the provider uses ISO
 * minor units) AND draw a symbol glyph for it on the device. Anything else is
 * omitted upstream rather than guessed. */
const SUPPORTED: Record<string, { exponent: number; symbol: string }> = {
  EUR: { exponent: 2, symbol: '€' },
  USD: { exponent: 2, symbol: '$' },
}

/* Matches the firmware display cap. The amount shown is clamped to this before
 * formatting so the <=15 ASCII-byte `valueText` postcondition holds for any
 * input (env overrides are arbitrary). The numeric metric `value` is untouched. */
export const DISPLAY_MAX = 99_999

const DEFAULT_LOCALE = 'nl-NL'

// Object.hasOwn (not `in`/bracket access) so inherited keys like `constructor`
// or `toString` are never treated as supported currencies.
function currencyMeta(currency: string): { exponent: number; symbol: string } | undefined {
  return Object.hasOwn(SUPPORTED, currency) ? SUPPORTED[currency] : undefined
}

export function isSupportedCurrency(currency: string): boolean {
  return currencyMeta(currency) !== undefined
}

// Unsupported currencies should be filtered out before display (see
// extraUsageMetric); if one ever reaches here we show the ISO code, never a
// misleading `$`.
export function currencySymbol(currency: string): string {
  return currencyMeta(currency)?.symbol ?? currency
}

export function minorToMajor(minor: number, currency: string): number | null {
  const meta = currencyMeta(currency)
  if (!meta) return null
  return minor / 10 ** meta.exponent
}

/* Format a major-unit amount as an ASCII-only string for the firmware font.
 * `style:'decimal' + numberingSystem:'latn' + useGrouping:false` guarantees the
 * output is digits plus at most one ASCII decimal separator (`.`/`,`) for every
 * locale — no Arabic digits, no NBSP grouping, no RTL controls.
 * `minimumFractionDigits:0` trims trailing zeros (1.20 -> "1,2", 1.00 -> "1"). */
export function formatAmount(amountMajor: number, currency: string, locale: string): string {
  const exponent = currencyMeta(currency)?.exponent ?? 2
  const safe = Number.isFinite(amountMajor)
    ? Math.min(Math.max(amountMajor, 0), DISPLAY_MAX)
    : 0
  const nf = new Intl.NumberFormat(locale, {
    style: 'decimal',
    numberingSystem: 'latn',
    useGrouping: false,
    minimumFractionDigits: 0,
    maximumFractionDigits: exponent,
  })
  const s = nf.format(safe)
  // Defensive: enforce the ASCII + length postcondition in code, not just docs.
  // Unreachable post-clamp with latn/no-grouping, but a hard guarantee for the wire.
  if (!/^[0-9]+([.,][0-9]+)?$/.test(s) || s.length > 15) {
    return safe.toFixed(exponent).replace(/\.?0+$/, '')
  }
  return s
}

/* Resolve DASHBOARD_LOCALE: validate syntax (getCanonicalLocales throws on
 * garbage) AND availability (new Intl.NumberFormat('xx-YY') does NOT throw — it
 * silently falls back — so supportedLocalesOf is required). Falls back to the
 * deterministic default rather than the runtime/container locale. */
export function resolveLocale(raw: string | undefined): string {
  const trimmed = raw?.trim()
  if (!trimmed) return DEFAULT_LOCALE
  let canonical: string
  try {
    canonical = Intl.getCanonicalLocales(trimmed)[0]
  } catch {
    console.warn(`[dashboard] DASHBOARD_LOCALE "${trimmed}" is not a valid locale; using ${DEFAULT_LOCALE}`)
    return DEFAULT_LOCALE
  }
  if (!canonical || Intl.NumberFormat.supportedLocalesOf([canonical]).length === 0) {
    console.warn(`[dashboard] DASHBOARD_LOCALE "${trimmed}" is unsupported; using ${DEFAULT_LOCALE}`)
    return DEFAULT_LOCALE
  }
  return canonical
}

export const DASHBOARD_LOCALE = resolveLocale(process.env.DASHBOARD_LOCALE)
