#pragma once
#include "esp_err.h"
#include "storage.h"

typedef struct {
    int issues;
    int prs;
    int dependabot;
    bool auth_error;
} github_data_t;

typedef struct {
    int used;
    int limit;
    int reset_in_seconds;
} rate_limit_t;

typedef struct {
    rate_limit_t five_hour;
    rate_limit_t weekly;
    bool auth_error;
} claude_data_t;

typedef struct {
    int short_pct;
    int long_pct;
    bool reached;
    int short_reset_in_seconds;
    int long_reset_in_seconds;
} codex_data_t;

typedef struct {
    int schema_version;
    bool github_present;
    github_data_t github;
    claude_data_t claude;
    codex_data_t codex;
    char updated_at[32];
    bool stale;
    bool offline;
} dashboard_data_t;

esp_err_t api_client_fetch_with_failover(dash_config_v2_t *cfg,
                                         int network_idx,
                                         bool prefer_last_success_api,
                                         dashboard_data_t *out,
                                         int *api_used_idx);
