/* In-memory last-hour usage tracking.
 *
 * Upstream providers only expose an instantaneous snapshot (the current
 * usedPercent of each rolling window), so "how much was used in the last hour"
 * has to be reconstructed here: we keep a tiny ring of recent samples per
 * service+window and diff the current reading against the one from ~1h ago.
 *
 * State lives only in this process's memory — no disk, and nothing on the
 * device (the firmware stays stateless and just renders the recentPercent we
 * emit, so there are no NVS writes). After a restart there is no hour of history
 * yet, so during warm-up we diff against the oldest sample we have (a shorter
 * window that grows to a full hour) instead of reporting nothing for an hour. */

const RECENT_WINDOW_MS = 60 * 60 * 1000 // diff against the reading ~1h ago
const RETENTION_MS = 90 * 60 * 1000 // keep ~90 min so the 1h lookup has a bound
const MAX_SAMPLES = 240 // hard per-key cap (defensive against tight poll loops)
// A real window reset jumps the reset instant by a whole window (>=5h); this
// tolerance only absorbs second-rounding jitter between fetches in one window.
const RESET_EPSILON_MS = 10 * 60 * 1000

type Sample = {
  ts: number
  usedPercent: number
  resetAt: number | null
}

const history = new Map<string, Sample[]>()

export function usageHistoryKey(serviceId: string, windowId: string): string {
  return `${serviceId}:${windowId}`
}

/* Test helper: drop all in-memory history. */
export function resetUsageHistory(): void {
  history.clear()
}

function resetInstantsEqual(a: number, b: number): boolean {
  return Math.abs(a - b) <= RESET_EPSILON_MS
}

function clampPercent(value: number): number {
  if (!Number.isFinite(value) || value < 0) return 0
  return value > 100 ? 100 : value
}

/* Record the current reading and return how much of `usedPercent` was accrued
 * in the last hour (0..usedPercent). `resetAt` is the window's absolute reset
 * instant in ms (now + resetInSeconds * 1000), used to detect a window reset. */
export function recordAndComputeRecent(
  key: string,
  usedPercent: number,
  resetAt: number | null,
  now: number,
): number {
  const samples = history.get(key) ?? []

  // Baseline = the newest sample that is already at least an hour old. Samples
  // are appended in chronological order, so the last one at/under the cutoff
  // wins and we can stop as soon as we pass it.
  const cutoff = now - RECENT_WINDOW_MS
  let baseline: Sample | undefined
  for (const sample of samples) {
    if (sample.ts <= cutoff) baseline = sample
    else break
  }

  // Warm-up fallback: before a full hour of history exists (e.g. right after a
  // restart) diff against the oldest sample we have, so the recent slice shows
  // up within a couple of polls and grows to a true one-hour window, instead of
  // staying empty for an hour.
  if (!baseline && samples.length > 0) baseline = samples[0]

  samples.push({ ts: now, usedPercent, resetAt })
  while (samples.length > 0 && samples[0].ts < now - RETENTION_MS) samples.shift()
  while (samples.length > MAX_SAMPLES) samples.shift()
  history.set(key, samples)

  if (!baseline) return 0

  // Window-reset guard: a tumbling window that reset between the baseline and
  // now drops usedPercent sharply (e.g. 95 -> 5), so a plain diff would go
  // negative and hide all the genuinely-recent usage. When the reset instant
  // moved, treat the whole current window as recent.
  if (
    baseline.resetAt != null &&
    resetAt != null &&
    !resetInstantsEqual(baseline.resetAt, resetAt)
  ) {
    return clampPercent(usedPercent)
  }

  return clampPercent(usedPercent - baseline.usedPercent)
}
