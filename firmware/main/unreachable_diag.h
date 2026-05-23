#pragma once

#include "storage.h"
#include <stdbool.h>
#include <stdint.h>

#define UNREACHABLE_REASON_MAX 9

typedef struct {
    uint8_t network_idx;
    char reason[UNREACHABLE_REASON_MAX];
} wifi_unreachable_row_t;

typedef struct {
    uint8_t row_count;
    wifi_unreachable_row_t rows[MAX_WIFI_NETWORKS];
} wifi_unreachable_diag_t;

typedef struct {
    uint8_t api_idx;
    char reason[UNREACHABLE_REASON_MAX];
} api_unreachable_row_t;

typedef struct {
    uint8_t row_count;
    api_unreachable_row_t rows[MAX_APIS_PER_NETWORK];
} api_unreachable_diag_t;

