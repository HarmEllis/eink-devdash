#include "reset_credits.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Option A: clockwise circular arrow, row-major 7x7 bitmap. */
const unsigned char reset_credit_icon_rows[7] = {
    0x1c, 0x22, 0x41, 0x41, 0x05, 0x06, 0x07,
};

static bool nonnegative_integer(const cJSON *value)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= 0 && value->valuedouble <= INT_MAX &&
           floor(value->valuedouble) == value->valuedouble;
}

void reset_credits_parse(const cJSON *value, reset_credits_t *out)
{
    memset(out, 0, sizeof(*out));
    out->next_expires_in_seconds = -1;
    if (!cJSON_IsObject(value)) return;
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(value, "availableCount");
    if (!nonnegative_integer(count)) return;
    out->present = true;
    out->count = (int)count->valuedouble;
    const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(value, "nextExpiresInSeconds");
    if (out->count > 0 && nonnegative_integer(expiry)) {
        out->next_expires_in_seconds = (int)expiry->valuedouble;
    }
}

void reset_credits_format_expiry(char *out, size_t size, int seconds, bool expanded)
{
    if (seconds <= 0) { if (size) out[0] = '\0'; return; }
    int hours = seconds / 3600;
    if (hours < 1) { snprintf(out, size, "<1h"); return; }
    if (hours < 24) { snprintf(out, size, "%dh", hours); return; }
    int days = hours / 24;
    if (days > 9) { snprintf(out, size, "9d+"); return; }
    if (expanded && hours % 24) snprintf(out, size, "%dd%dh", days, hours % 24);
    else snprintf(out, size, "%dd", days);
}

static int badge_width(const reset_credit_layout_t *layout, const char *expiry)
{
    /* The count's trailing font column supplies the 1px count/icon gap. */
    return (int)strlen(layout->count_text) * 6 + RESET_CREDIT_ICON_WIDTH +
           (expiry[0] ? 1 + (int)strlen(expiry) * 6 : 0);
}

reset_credit_layout_t reset_credits_layout(const reset_credits_t *credits,
                                          size_t label_length, int label_x,
                                          int label_font_width, int right)
{
    reset_credit_layout_t layout = { .label_length = label_length };
    if (!credits || !credits->present || credits->count < 0) return layout;
    if (credits->count > 99) snprintf(layout.count_text, sizeof(layout.count_text), "99+");
    else snprintf(layout.count_text, sizeof(layout.count_text), "%d", credits->count);
    if (credits->count > 0) reset_credits_format_expiry(layout.expiry_text,
        sizeof(layout.expiry_text), credits->next_expires_in_seconds, false);
    int budget = badge_width(&layout, credits->count > 0 ? "23h" : "") + 6;
    int left = label_x + (int)layout.label_length * label_font_width;
    if (right - left < budget && layout.label_length > 3) layout.label_length = 3;
    left = label_x + (int)layout.label_length * label_font_width;
    while (right - left < budget && layout.label_length > 1) {
        layout.label_length--;
        left = label_x + (int)layout.label_length * label_font_width;
    }
    char expanded[8] = "";
    if (credits->count > 0) reset_credits_format_expiry(expanded, sizeof(expanded),
        credits->next_expires_in_seconds, true);
    if (badge_width(&layout, expanded) + 6 <= right - left) {
        snprintf(layout.expiry_text, sizeof(layout.expiry_text), "%s", expanded);
    }
    if (badge_width(&layout, layout.expiry_text) + 6 > right - left) {
        layout.expiry_text[0] = '\0';
    }
    int width = badge_width(&layout, layout.expiry_text);
    if (width + 6 > right - left) {
        layout.label_length = label_length;
        return layout;
    }
    layout.visible = true;
    layout.badge_x = left + (right - left - width) / 2;
    layout.icon_x = layout.badge_x + (int)strlen(layout.count_text) * 6;
    layout.expiry_x = layout.icon_x + RESET_CREDIT_ICON_WIDTH + 1;
    return layout;
}
