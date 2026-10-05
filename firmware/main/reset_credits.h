#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

typedef struct {
    bool present;
    int count;
    int next_expires_in_seconds; /* -1 = unknown; relative snapshot time */
} reset_credits_t;

typedef struct {
    size_t label_length;
    bool visible;
    int badge_x;
    int icon_x;
    int expiry_x;
    char count_text[4];
    char expiry_text[8];
} reset_credit_layout_t;

#define RESET_CREDIT_ICON_WIDTH 7
extern const unsigned char reset_credit_icon_rows[7];

void reset_credits_parse(const cJSON *value, reset_credits_t *out);
void reset_credits_format_expiry(char *out, size_t size, int seconds, bool expanded);
/* Coordinates are relative to the provider title. right is the reserved left
 * edge of the automatic reset times/hourglass, not the panel's right edge. */
reset_credit_layout_t reset_credits_layout(const reset_credits_t *credits,
                                          size_t label_length, int label_x,
                                          int label_font_width, int right);
