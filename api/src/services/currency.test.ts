import { test } from 'node:test'
import assert from 'node:assert/strict'

import {
  DISPLAY_MAX,
  currencySymbol,
  formatAmount,
  isSupportedCurrency,
  minorToMajor,
  resolveLocale,
} from './currency.js'

const ASCII_AMOUNT = /^[0-9]+([.,][0-9]+)?$/

test('minorToMajor converts only supported currencies', () => {
  assert.equal(minorToMajor(91, 'EUR'), 0.91)
  assert.equal(minorToMajor(1700, 'USD'), 17)
  assert.equal(minorToMajor(91, 'GBP'), null)
  assert.equal(isSupportedCurrency('EUR'), true)
  assert.equal(isSupportedCurrency('JPY'), false)
})

test('inherited object keys are never treated as supported currencies', () => {
  for (const key of ['constructor', 'toString', '__proto__', 'hasOwnProperty']) {
    assert.equal(isSupportedCurrency(key), false, key)
    assert.equal(minorToMajor(100, key), null, key)
    // No misleading `$`: an unsupported code maps to itself, not a symbol.
    assert.equal(currencySymbol(key), key, key)
  }
})

test('currencySymbol maps supported currencies', () => {
  assert.equal(currencySymbol('EUR'), '€')
  assert.equal(currencySymbol('USD'), '$')
})

test('formatAmount uses the locale decimal separator and trims trailing zeros', () => {
  assert.equal(formatAmount(0.91, 'EUR', 'nl-NL'), '0,91')
  assert.equal(formatAmount(0.91, 'EUR', 'en-US'), '0.91')
  assert.equal(formatAmount(1.2, 'EUR', 'nl-NL'), '1,2')
  assert.equal(formatAmount(1.0, 'EUR', 'nl-NL'), '1')
  assert.equal(formatAmount(12.34, 'USD', 'en-US'), '12.34')
  assert.equal(formatAmount(5, 'USD', 'nl-NL'), '5')
})

test('formatAmount never groups thousands (no NBSP/space separators)', () => {
  for (const locale of ['nl-NL', 'fr-FR', 'en-US', 'de-DE']) {
    const s = formatAmount(12345.6, 'EUR', locale)
    assert.ok(ASCII_AMOUNT.test(s), `${locale} -> ${JSON.stringify(s)} should be ASCII digits + one separator`)
    assert.ok(s.length <= 15)
  }
})

test('formatAmount clamps negatives, NaN and Infinity to 0', () => {
  assert.equal(formatAmount(-5, 'EUR', 'nl-NL'), '0')
  assert.equal(formatAmount(Number.NaN, 'EUR', 'nl-NL'), '0')
  assert.equal(formatAmount(Number.POSITIVE_INFINITY, 'EUR', 'nl-NL'), '0')
})

test('formatAmount clamps huge values to DISPLAY_MAX and stays <=15 ASCII bytes', () => {
  for (const huge of [1e15, Number.MAX_VALUE, DISPLAY_MAX + 1]) {
    const s = formatAmount(huge, 'EUR', 'nl-NL')
    assert.ok(ASCII_AMOUNT.test(s), `${huge} -> ${JSON.stringify(s)}`)
    assert.ok(s.length <= 15)
    assert.equal(s, String(DISPLAY_MAX))
  }
})

test('formatAmount rounds at the supported precision near the cap', () => {
  // maxFractionDigits = 2 (EUR), so values round rather than overflow the cap.
  assert.equal(formatAmount(99999.999, 'EUR', 'en-US'), '99999')
  assert.equal(formatAmount(12.345, 'USD', 'en-US'), '12.35')
})

test('resolveLocale validates syntax and availability, else defaults to nl-NL', () => {
  assert.equal(resolveLocale(undefined), 'nl-NL')
  assert.equal(resolveLocale('  '), 'nl-NL')
  assert.equal(resolveLocale('en-US'), 'en-US')
  // Canonicalisation normalises casing.
  assert.equal(resolveLocale('NL-nl'), 'nl-NL')
  // Syntactically invalid -> throws internally -> default.
  assert.equal(resolveLocale('not a locale!'), 'nl-NL')
  // Syntactically valid but unsupported region silently falls back in Intl, so
  // supportedLocalesOf must catch it -> default.
  assert.equal(resolveLocale('xx-YY'), 'nl-NL')
})
