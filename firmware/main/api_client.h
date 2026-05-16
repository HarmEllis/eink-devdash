#pragma once
#include "esp_err.h"
#include "storage.h"

typedef struct {
    int issues;
    int prs;
    int dependabot;
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
    int daily_used;
    int daily_limit;
} codex_data_t;

typedef struct {
    int schema_version;
    github_data_t github;
    claude_data_t claude;
    codex_data_t codex;
    char updated_at[32];
    bool stale;
    bool offline;
} dashboard_data_t;

esp_err_t api_client_fetch(const dash_config_t *cfg, dashboard_data_t *out);
