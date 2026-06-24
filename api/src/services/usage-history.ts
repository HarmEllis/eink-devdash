/* In-memory recent-usage tracking.
 *
 * Upstream providers only expose an instantaneous snapshot (the current
 * usedPercent of each rolling window), so "how much was used recently" has to
 * be reconstructed here: we keep a tiny ring of recent samples per
 * service+window and diff the current reading against the one from before a
 * caller-supplied cutoff. The cutoff is what makes the slice mean either "the
 * last hour" (short windows) or "today" / since local midnight (the 7d window);
 * this module stays timezone-agnostic and only the caller picks the cutoff.
 *
 * State lives only in this process's memory — no disk, and nothing on the
 * device (the firmware stays stateless and just renders the recentPercent we
 * emit, so there are no NVS writes). After a restart there is no history before
 * the cutoff yet, so during warm-up we diff against the oldest sample we have (a
 * shorter window that grows to the full one) instead of reporting nothing. */

const RECENT_WINDOW_MS = 60 * 60 * 1000 // default cutoff: diff against the reading ~1h ago
// The history is shared across all clients (multiple devices, browser refreshes,
// monitoring scripts), so the effective poll rate can far exceed 1/min. Coalesce
// readings closer together than this into one sample so a day-long lookup cannot
// fill MAX_SAMPLES and evict the pre-cutoff baseline before the day is over.
const MIN_SAMPLE_INTERVAL_MS = 60 * 1000
const RETENTION_SLACK_MS = 30 * 60 * 1000 // keep a little history before the cutoff
// Defensive per-key cap. With coalescing the real bound is the cutoff span:
// ~26h (a DST-long day plus slack) at 1/min -> ~1560, so 1600 leaves headroom
// and the cap can no longer evict the baseline at the supported poll cadence.
const MAX_SAMPLES = 1600
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
 * since `cutoffMs` (0..usedPercent). `resetAt` is the window's absolute reset
 * instant in ms (now + resetInSeconds * 1000), used to detect a window reset.
 * `cutoffMs` defaults to ~1h ago (the last-hour slice); the caller passes the
 * start of the local day for the "today" slice on the 7d window. */
export function recordAndComputeRecent(
  key: string,
  usedPercent: number,
  resetAt: number | null,
  now: number,
  cutoffMs: number = now - RECENT_WINDOW_MS,
): number {
  const samples = history.get(key) ?? []

  // Baseline = the newest sample already at/under the cutoff. Samples are kept
  // in chronological order, so the last one at/under it wins and we can stop as
  // soon as we pass it. Snapshot its values now: coalescing below may mutate the
  // newest sample, which can alias the warm-up baseline.
  let baseline: Sample | undefined
  for (const sample of samples) {
    if (sample.ts <= cutoffMs) baseline = sample
    else break
  }

  // Warm-up fallback: before any history older than the cutoff exists (e.g.
  // right after a restart) diff against the oldest sample we have, so the recent
  // slice shows up within a couple of polls and grows to the full window,
  // instead of staying empty.
  if (!baseline && samples.length > 0) baseline = samples[0]
  const hadBaseline = baseline !== undefined
  const baselineUsedPercent = baseline?.usedPercent ?? 0
  const baselineResetAt = baseline?.resetAt ?? null

  // Record the current reading, coalescing when the newest sample is younger
  // than the min interval so a fast/multi-client poll rate cannot balloon the
  // ring (see MIN_SAMPLE_INTERVAL_MS). We update the sample's value in place but
  // KEEP its timestamp (the bucket anchor): bumping the timestamp to `now` would
  // keep the newest sample perpetually under the interval, so nothing would ever
  // age out and we would retain only one sample, leaving the baseline tracking
  // "now" and the recent slice stuck at ~0. Baselines only read older samples,
  // and the current reading is always the `usedPercent` arg, so the <=60s of
  // timestamp skew this introduces is harmless.
  const newest = samples[samples.length - 1]
  if (newest && now - newest.ts < MIN_SAMPLE_INTERVAL_MS) {
    newest.usedPercent = usedPercent
    newest.resetAt = resetAt
  } else {
    samples.push({ ts: now, usedPercent, resetAt })
  }

  // Retention is derived from the cutoff: keep just enough history to hold the
  // baseline (back to the cutoff plus slack). The MAX_SAMPLES cap is only a
  // defensive memory bound; coalescing keeps the count well under it.
  const retentionFloor = cutoffMs - RETENTION_SLACK_MS
  while (samples.length > 0 && samples[0].ts < retentionFloor) samples.shift()
  while (samples.length > MAX_SAMPLES) samples.shift()
  history.set(key, samples)

  if (!hadBaseline) return 0

  // Window-reset guard: a tumbling window that reset between the baseline and
  // now drops usedPercent sharply (e.g. 95 -> 5), so a plain diff would go
  // negative and hide all the genuinely-recent usage. When the reset instant
  // moved, treat the whole current window as recent.
  if (
    baselineResetAt != null &&
    resetAt != null &&
    !resetInstantsEqual(baselineResetAt, resetAt)
  ) {
    return clampPercent(usedPercent)
  }

  return clampPercent(usedPercent - baselineUsedPercent)
}
