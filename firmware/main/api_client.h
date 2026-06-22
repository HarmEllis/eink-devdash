#pragma once
#include "esp_err.h"
#include "storage.h"
#include "unreachable_diag.h"

typedef struct {
    int issues;
    int prs;
    int dependabot;
    int notifications;
    bool notifications_present;
    bool auth_error;
    bool service_error;
} github_data_t;

/* Extra-usage ("usage credits") spend, rendered as a currency row:
   [symbol] [bar = percent] [amount]. `percent_present` is false for the env
   override path (no percent source) → the device uses an amount-capped bar. */
typedef struct {
    bool present;
    double amount;          /* major currency units spent (e.g. 0.91) */
    int percent;            /* 0..100 share of the monthly cap consumed */
    bool percent_present;   /* false → fall back to an amount-capped bar */
    char currency[4];       /* Supported ISO-4217 code: "EUR" or "USD" */
    char value_text[16];    /* API-preformatted, locale-aware amount string
                               (e.g. "0,91"); empty → format `amount` locally */
} extra_usage_t;

#define DASH_MAX_USAGE_SERVICES 4
#define DASH_USAGE_LABEL_LEN 11
#define DASH_WINDOW_LABEL_LEN 3

typedef struct {
    char label[DASH_WINDOW_LABEL_LEN];
    int used_pct;
    int recent_pct;        /* usage accrued in the last hour (0..used_pct), grey slice */
    int tick_pct;          /* recommended daily-limit marker (0..100), -1 = none */
    int reset_in_seconds;
    bool reached;
} usage_window_data_t;

typedef struct {
    char label[DASH_USAGE_LABEL_LEN];
    char icon[12];
    usage_window_data_t windows[2];
    int window_count;
    bool service_error;
    extra_usage_t extra_usage;
} usage_service_data_t;

typedef struct {
    int schema_version;
    bool github_present;
    github_data_t github;
    usage_service_data_t usage[DASH_MAX_USAGE_SERVICES];
    int usage_count;
    char updated_at[32];
    /* Full local wall-clock ISO timestamp ("YYYY-MM-DDTHH:MM:SS") from the
       API's updatedAtLocalIso field. Used to set the RTC clock so the
       per-network quiet-hours window can be evaluated against local time.
       Empty when the API did not provide it. */
    char updated_at_iso[24];
    bool stale;
    bool offline;
} dashboard_data_t;

esp_err_t api_client_fetch_with_failover(dash_config_v2_t *cfg,
                                         int network_idx,
                                         bool prefer_last_success_api,
                                         dashboard_data_t *out,
                                         int *api_used_idx,
                                         api_unreachable_diag_t *diag);
