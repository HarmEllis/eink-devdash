#include "timekeep.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "timekeep";

/* Plausibility floor: the firmware shipped well after 2024, so any year below
 * this means the RTC was never set (cold boot) or carries garbage. */
#define TIMEKEEP_MIN_YEAR 2024

/* Seconds since the Unix epoch for a UTC calendar date/time, using Howard
 * Hinnant's days-from-civil algorithm. Avoids timegm(), which newlib does not
 * declare under ESP-IDF's feature macros. */
static time_t epoch_from_utc(int year, int mon, int day,
                             int hour, int min, int sec)
{
    int y = year - (mon <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 +
                              day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + hour * 3600 + min * 60 + sec;
}

static bool parse_2(const char *s, int *out)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    *out = (s[0] - '0') * 10 + (s[1] - '0');
    return true;
}

static bool parse_4(const char *s, int *out)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

bool timekeep_set_from_iso(const char *iso)
{
    if (!iso) return false;
    /* Need at least "YYYY-MM-DDTHH:MM:SS" (19 chars). */
    if (strlen(iso) < 19) return false;
    if (iso[4] != '-' || iso[7] != '-' ||
        (iso[10] != 'T' && iso[10] != ' ') ||
        iso[13] != ':' || iso[16] != ':') {
        return false;
    }

    int year, mon, day, hour, min, sec;
    if (!parse_4(iso + 0, &year) ||
        !parse_2(iso + 5, &mon) ||
        !parse_2(iso + 8, &day) ||
        !parse_2(iso + 11, &hour) ||
        !parse_2(iso + 14, &min) ||
        !parse_2(iso + 17, &sec)) {
        return false;
    }
    if (year < TIMEKEEP_MIN_YEAR || mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60) {
        return false;
    }

    /* Convert as if the fields were UTC. The stored timestamp is offset-less
       local wall time, so treating it as UTC keeps gmtime()/localtime() math
       offset-free. */
    time_t epoch = epoch_from_utc(year, mon, day, hour, min, sec);
    if (epoch < 0) return false;

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return false;
    }
    ESP_LOGI(TAG, "Clock set from ISO: %04d-%02d-%02d %02d:%02d:%02d (local)",
             year, mon, day, hour, min, sec);
    return true;
}

bool timekeep_time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (tm.tm_year + 1900) >= TIMEKEEP_MIN_YEAR;
}

bool timekeep_now_minute_of_day(int *out)
{
    if (!out) return false;
    if (!timekeep_time_is_valid()) return false;
    time_t now = time(NULL);
    struct tm tm;
    /* Time was stored as UTC-encoded local wall time, so gmtime gives back the
       same local fields. */
    gmtime_r(&now, &tm);
    *out = tm.tm_hour * 60 + tm.tm_min;
    return true;
}

bool timekeep_minute_in_window(int now, int start, int end)
{
    if (start == end) return false;            /* empty window */
    if (start < end) return now >= start && now < end;
    return now >= start || now < end;          /* wraps midnight */
}

int timekeep_minutes_until(int now, int target)
{
    int d = (target - now) % 1440;
    if (d < 0) d += 1440;
    return d;
}
