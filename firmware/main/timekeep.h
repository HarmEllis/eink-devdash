#pragma once
#include <stdbool.h>

/* Wall-clock helpers for the quiet-hours feature.
 *
 * The device has no SNTP. It derives local wall time from the API's
 * updatedAtLocalIso field after a successful fetch and stores it in the RTC
 * system clock, which keeps running across deep-sleep timer wakes (it is lost
 * on cold boot / power loss). The window is evaluated against that clock.
 *
 * The stored timestamp is offset-less local wall time; timekeep treats it as
 * UTC internally so localtime() math stays offset-free and DST-agnostic — the
 * server already encodes local time, which is exactly what the user schedules
 * the window against. */

/* Parse "YYYY-MM-DDTHH:MM:SS" (a trailing 'Z' or timezone offset is ignored)
 * and set the system clock from it. Returns true on success, false when the
 * string is malformed or implausible (year < 2024). */
bool timekeep_set_from_iso(const char *iso);

/* True when the system clock has been set to a plausible wall time
 * (year >= 2024) — i.e. a fetch has happened since the last cold boot. */
bool timekeep_time_is_valid(void);

/* If the clock is valid, write the current local minute-of-day
 * (hour*60 + minute, range [0,1439]) to *out and return true; else false. */
bool timekeep_now_minute_of_day(int *out);

/* True when minute-of-day `now` falls inside [start, end). Overnight windows
 * wrap midnight (start > end). start == end is treated as an empty window
 * (always false). All arguments are minutes-of-day in [0,1439]. */
bool timekeep_minute_in_window(int now, int start, int end);

/* Forward distance in minutes from `now` to `target`, wrapping midnight, in
 * [0,1439]. Used to size the deep-sleep chunk until the window ends. */
int timekeep_minutes_until(int now, int target);
