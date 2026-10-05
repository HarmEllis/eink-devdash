#include "unity.h"
#include "reset_credits.h"
#include <limits.h>
#include <string.h>

static void test_reset_credit_json_distinguishes_zero_and_unknown(void)
{
    reset_credits_t result = { .present = true, .count = 99 };
    reset_credits_parse(NULL, &result);
    TEST_ASSERT_FALSE(result.present);
    cJSON *value = cJSON_Parse("{\"availableCount\":0,\"nextExpiresInSeconds\":null}");
    reset_credits_parse(value, &result);
    TEST_ASSERT_TRUE(result.present);
    TEST_ASSERT_EQUAL_INT(0, result.count);
    TEST_ASSERT_EQUAL_INT(-1, result.next_expires_in_seconds);
    cJSON_Delete(value);
    value = cJSON_Parse("{\"availableCount\":2,\"nextExpiresInSeconds\":39600}");
    reset_credits_parse(value, &result);
    TEST_ASSERT_TRUE(result.present);
    TEST_ASSERT_EQUAL_INT(2, result.count);
    TEST_ASSERT_EQUAL_INT(39600, result.next_expires_in_seconds);
    cJSON_Delete(value);
}

static void test_reset_credit_json_rejects_overflow_and_fractional_counts(void)
{
    const char *invalid[] = { "-1", "1.5", "2147483648", "1e99", "\"2\"", "null" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cJSON *value = cJSON_CreateObject();
        cJSON_AddItemToObject(value, "availableCount", cJSON_Parse(invalid[i]));
        reset_credits_t result;
        reset_credits_parse(value, &result);
        TEST_ASSERT_FALSE(result.present);
        cJSON_Delete(value);
    }
    cJSON *value = cJSON_Parse("{\"availableCount\":2,\"nextExpiresInSeconds\":1e99}");
    reset_credits_t result;
    reset_credits_parse(value, &result);
    TEST_ASSERT_TRUE(result.present);
    TEST_ASSERT_EQUAL_INT(-1, result.next_expires_in_seconds);
    cJSON_Delete(value);
}

static void test_reset_credit_expiry_boundaries(void)
{
    const int seconds[] = { -1, 0, 1, 3599, 3600, 86399, 86400, 298800, 864000, INT_MAX };
    const char *compact[] = { "", "", "<1h", "<1h", "1h", "23h", "1d", "3d", "9d+", "9d+" };
    char text[8];
    for (unsigned i = 0; i < sizeof(seconds) / sizeof(seconds[0]); i++) {
        reset_credits_format_expiry(text, sizeof(text), seconds[i], false);
        TEST_ASSERT_EQUAL_STRING(compact[i], text);
    }
    reset_credits_format_expiry(text, sizeof(text), 298800, true);
    TEST_ASSERT_EQUAL_STRING("3d11h", text);
}

static void test_reset_credit_headers_fit_grid_row_and_hero(void)
{
    const int counts[] = { 0, 2, 12, 99, 100, INT_MAX };
    const int seconds[] = { -1, 1, 39600, 298800, INT_MAX };
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        for (unsigned j = 0; j < sizeof(seconds) / sizeof(seconds[0]); j++) {
            reset_credits_t credits = { true, counts[i], seconds[j] };
            const int rights[] = { 138 - 66, 288 - 66 - 10, 288 - 66 - 10 };
            const int starts[] = { 11, 11, 29 };
            const int fonts[] = { 6, 6, 12 };
            for (int mode = 0; mode < 3; mode++) {
                reset_credit_layout_t layout = reset_credits_layout(&credits, 6,
                    starts[mode], fonts[mode], rights[mode]);
                TEST_ASSERT_TRUE(layout.visible);
                TEST_ASSERT_GREATER_OR_EQUAL_INT(starts[mode] + (int)layout.label_length * fonts[mode] + 3,
                    layout.badge_x);
                int end = layout.expiry_text[0]
                    ? layout.expiry_x + (int)strlen(layout.expiry_text) * 6
                    : layout.icon_x + RESET_CREDIT_ICON_WIDTH;
                TEST_ASSERT_LESS_OR_EQUAL_INT(rights[mode] - 3, end);
                if (counts[i] == 0) TEST_ASSERT_EQUAL_STRING("", layout.expiry_text);
            }
        }
    }
    reset_credits_t credits = { true, 2, 39600 };
    reset_credit_layout_t before = reset_credits_layout(&credits, 6, 11, 6, 72);
    credits.next_expires_in_seconds = 9 * 3600;
    reset_credit_layout_t after = reset_credits_layout(&credits, 6, 11, 6, 72);
    TEST_ASSERT_EQUAL_INT(before.label_length, after.label_length);
    credits.present = false;
    TEST_ASSERT_FALSE(reset_credits_layout(&credits, 6, 11, 6, 72).visible);
    credits.present = true;
    reset_credit_layout_t hidden = reset_credits_layout(&credits, 6, 11, 6, 12);
    TEST_ASSERT_FALSE(hidden.visible);
    TEST_ASSERT_EQUAL_INT(6, hidden.label_length);
}

void run_reset_credit_tests(void)
{
    RUN_TEST(test_reset_credit_json_distinguishes_zero_and_unknown);
    RUN_TEST(test_reset_credit_json_rejects_overflow_and_fractional_counts);
    RUN_TEST(test_reset_credit_expiry_boundaries);
    RUN_TEST(test_reset_credit_headers_fit_grid_row_and_hero);
}
