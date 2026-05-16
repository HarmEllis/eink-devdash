#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NVS_NAMESPACE "devdash"
#define NVS_SCHEMA_VERSION 1

typedef struct {
    char api_url[256];
    char device_token[64];
    uint8_t refresh_min;   /* 3–60 */
    bool provisioned;
    bool last_red_state;
    uint8_t bw_fast_cycle_count;
} dash_config_t;

void storage_init(void);
void storage_load(dash_config_t *cfg);
void storage_save(const dash_config_t *cfg);
void storage_erase(void);
