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

typedef struct {
    int used;
    int limit;
    int reset_in_seconds;
} rate_limit_t;

/* Extra-usage ("usage credits") spend, rendered as a currency row:
   [symbol] [bar = percent] [amount]. `percent_present` is false for the env
   override path (no percent source) → the device uses an amount-capped bar. */
typedef struct {
    bool present;
    double amount;          /* major currency units spent (e.g. 0.91) */
    int percent;            /* 0..100 share of the monthly cap consumed */
    bool percent_present;   /* false → fall back to an amount-capped bar */
    char currency[4];       /* ISO-4217 code, e.g. "EUR"/"USD"; empty → "$" */
} extra_usage_t;

typedef struct {
    rate_limit_t five_hour;
    rate_limit_t weekly;
    extra_usage_t extra_usage;
    bool auth_error;
} claude_data_t;

typedef struct {
    int short_pct;
    int long_pct;
    bool reached;
    int short_reset_in_seconds;
    int long_reset_in_seconds;
    extra_usage_t extra_usage;
} codex_data_t;

typedef struct {
    int schema_version;
    bool github_present;
    github_data_t github;
    claude_data_t claude;
    codex_data_t codex;
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
