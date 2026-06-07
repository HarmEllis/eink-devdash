/*
 * Host unit tests for the pure OTA trust-anchor helpers in
 * firmware/main/ota_version.c. Built and run on the ESP-IDF `linux` target
 * (see firmware/test/README.md) so the parsing/comparison logic can be
 * exercised without flashing hardware.
 */
#include <stdlib.h>

#include "unity.h"
#include "ota_version.h"
#include "runtime_policy.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- ota_download_url_is_canonical -------------------------------------- */

static const char *CANONICAL_V040 =
    "https://github.com/HarmEllis/eink-devdash/releases/download/v0.4.0/eink-devdash.bin";

static void test_canonical_url_accepts_exact_match(void)
{
    TEST_ASSERT_TRUE(ota_download_url_is_canonical(CANONICAL_V040, "v0.4.0"));
}

static void test_canonical_url_rejects_other_repo(void)
{
    TEST_ASSERT_FALSE(ota_download_url_is_canonical(
        "https://github.com/attacker/evil/releases/download/v0.4.0/eink-devdash.bin",
        "v0.4.0"));
}

static void test_canonical_url_rejects_other_asset(void)
{
    TEST_ASSERT_FALSE(ota_download_url_is_canonical(
        "https://github.com/HarmEllis/eink-devdash/releases/download/v0.4.0/evil.bin",
        "v0.4.0"));
}

static void test_canonical_url_rejects_version_mismatch(void)
{
    /* URL embeds v0.4.0 but the manifest claims v0.5.0. */
    TEST_ASSERT_FALSE(ota_download_url_is_canonical(CANONICAL_V040, "v0.5.0"));
}

static void test_canonical_url_rejects_non_https_scheme(void)
{
    TEST_ASSERT_FALSE(ota_download_url_is_canonical(
        "http://github.com/HarmEllis/eink-devdash/releases/download/v0.4.0/eink-devdash.bin",
        "v0.4.0"));
}

static void test_canonical_url_rejects_null(void)
{
    TEST_ASSERT_FALSE(ota_download_url_is_canonical(NULL, "v0.4.0"));
    TEST_ASSERT_FALSE(ota_download_url_is_canonical(CANONICAL_V040, NULL));
}

/* ---- ota_version_is_newer ----------------------------------------------- */

static void test_version_accepts_strictly_newer(void)
{
    TEST_ASSERT_TRUE(ota_version_is_newer("v0.4.0", "v0.3.1"));
    TEST_ASSERT_TRUE(ota_version_is_newer("v1.0.0", "v0.9.9"));
    TEST_ASSERT_TRUE(ota_version_is_newer("v0.3.2", "v0.3.1"));
}

static void test_version_rejects_equal(void)
{
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.3.1", "v0.3.1"));
}

static void test_version_rejects_older(void)
{
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.3.0", "v0.3.1"));
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.9.0", "v0.10.0"));
}

static void test_version_rejects_git_describe_equal_base(void)
{
    /* Running a dev build past v0.3.1; latest is exactly v0.3.1 -> not newer. */
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.3.1", "v0.3.1-2-gabc1234"));
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.3.1", "v0.3.1-2-gabc1234-dirty"));
}

static void test_version_numeric_not_lexical_9_to_10(void)
{
    /* Lexical compare would call "0.9.0" > "0.10.0"; numeric must not. */
    TEST_ASSERT_TRUE(ota_version_is_newer("v0.10.0", "v0.9.0"));
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.9.0", "v0.10.0"));
}

static void test_version_tolerates_running_without_v(void)
{
    /* Only the locally embedded running version may omit the leading 'v'. */
    TEST_ASSERT_TRUE(ota_version_is_newer("v0.4.0", "0.3.1"));
    TEST_ASSERT_TRUE(ota_version_is_newer("v0.4.0", "0.1.0-3-gabc1234-dirty"));
}

static void test_version_fails_closed_on_malformed_latest(void)
{
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4", "v0.3.1"));      /* too few parts */
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4.0.1", "v0.3.1"));  /* too many parts */
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4.x", "v0.3.1"));    /* non-digit */
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.04.0", "v0.3.1"));   /* leading zero */
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4.0-rc1", "v0.3.1")); /* suffix on latest */
    TEST_ASSERT_FALSE(ota_version_is_newer("", "v0.3.1"));          /* empty */
    TEST_ASSERT_FALSE(ota_version_is_newer(NULL, "v0.3.1"));
}

static void test_version_fails_closed_on_unprefixed_latest(void)
{
    /* The mandatory 'v' applies to latest; an unprefixed tag would 404. */
    TEST_ASSERT_FALSE(ota_version_is_newer("0.4.0", "v0.3.1"));
}

static void test_version_fails_closed_on_overflow(void)
{
    /* 2^32 = 4294967296 overflows the uint32_t component parser. */
    TEST_ASSERT_FALSE(ota_version_is_newer("v4294967296.0.0", "v0.3.1"));
    /* 2^32-1 is the largest accepted component; still strictly newer. */
    TEST_ASSERT_TRUE(ota_version_is_newer("v4294967295.0.0", "v0.3.1"));
}

static void test_version_fails_closed_on_malformed_running(void)
{
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4.0", "garbage"));
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4.0", ""));
    TEST_ASSERT_FALSE(ota_version_is_newer("v0.4.0", NULL));
}

static void test_clock_applies_only_fresh_nonempty_iso(void)
{
    TEST_ASSERT_TRUE(clock_should_apply("2026-06-07T12:00:00", false));
    TEST_ASSERT_FALSE(clock_should_apply("2026-06-07T12:00:00", true));
    TEST_ASSERT_FALSE(clock_should_apply("", false));
    TEST_ASSERT_FALSE(clock_should_apply(NULL, false));
}

static void test_relay_url_detection_uses_device_path(void)
{
    TEST_ASSERT_TRUE(api_url_is_relay("https://relay.example/d/abc/dashboard"));
    TEST_ASSERT_TRUE(api_url_is_relay("https://relay.example/d/abc"));
    TEST_ASSERT_FALSE(api_url_is_relay("http://192.168.1.5:3000/dashboard"));
    TEST_ASSERT_FALSE(api_url_is_relay("https://api.example/dashboard"));
    TEST_ASSERT_FALSE(api_url_is_relay(""));
    TEST_ASSERT_FALSE(api_url_is_relay(NULL));
}

static void test_refresh_minimum_allows_one_minute_for_bw_at_least_two_partials(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, dashboard_refresh_input_minimum(true));
    TEST_ASSERT_EQUAL_UINT8(3, dashboard_refresh_input_minimum(false));
    TEST_ASSERT_EQUAL_UINT8(1, dashboard_refresh_minimum(true, 2));
    TEST_ASSERT_EQUAL_UINT8(1, dashboard_refresh_minimum(true, 3));
    TEST_ASSERT_EQUAL_UINT8(1, dashboard_refresh_minimum(true, 100));
    TEST_ASSERT_EQUAL_UINT8(3, dashboard_refresh_minimum(true, 1));
    TEST_ASSERT_EQUAL_UINT8(3, dashboard_refresh_minimum(false, 2));
}

static void test_refresh_config_rejects_unsafe_one_minute_combinations(void)
{
    TEST_ASSERT_TRUE(dashboard_refresh_config_is_valid(1, true, 2));
    TEST_ASSERT_TRUE(dashboard_refresh_config_is_valid(1, true, 3));
    TEST_ASSERT_TRUE(dashboard_refresh_config_is_valid(1, true, 100));
    TEST_ASSERT_TRUE(dashboard_refresh_config_is_valid(3, true, 1));
    TEST_ASSERT_TRUE(dashboard_refresh_config_is_valid(3, false, 2));
    TEST_ASSERT_FALSE(dashboard_refresh_config_is_valid(1, true, 1));
    TEST_ASSERT_FALSE(dashboard_refresh_config_is_valid(1, false, 2));
    TEST_ASSERT_FALSE(dashboard_refresh_config_is_valid(61, true, 2));
}

static void test_offline_partial_policy_enforces_both_caps(void)
{
    TEST_ASSERT_TRUE(offline_partial_refresh_allowed(0, 2, 1, 1440));
    TEST_ASSERT_TRUE(offline_partial_refresh_allowed(1, 2, 2, 1440));
    TEST_ASSERT_FALSE(offline_partial_refresh_allowed(2, 2, 3, 1440));
    TEST_ASSERT_FALSE(offline_partial_refresh_allowed(0, 0, 1, 1440));
    TEST_ASSERT_FALSE(offline_partial_refresh_allowed(0, 2, 24, 24));
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_canonical_url_accepts_exact_match);
    RUN_TEST(test_canonical_url_rejects_other_repo);
    RUN_TEST(test_canonical_url_rejects_other_asset);
    RUN_TEST(test_canonical_url_rejects_version_mismatch);
    RUN_TEST(test_canonical_url_rejects_non_https_scheme);
    RUN_TEST(test_canonical_url_rejects_null);

    RUN_TEST(test_version_accepts_strictly_newer);
    RUN_TEST(test_version_rejects_equal);
    RUN_TEST(test_version_rejects_older);
    RUN_TEST(test_version_rejects_git_describe_equal_base);
    RUN_TEST(test_version_numeric_not_lexical_9_to_10);
    RUN_TEST(test_version_tolerates_running_without_v);
    RUN_TEST(test_version_fails_closed_on_malformed_latest);
    RUN_TEST(test_version_fails_closed_on_unprefixed_latest);
    RUN_TEST(test_version_fails_closed_on_overflow);
    RUN_TEST(test_version_fails_closed_on_malformed_running);
    RUN_TEST(test_clock_applies_only_fresh_nonempty_iso);
    RUN_TEST(test_relay_url_detection_uses_device_path);
    RUN_TEST(test_refresh_minimum_allows_one_minute_for_bw_at_least_two_partials);
    RUN_TEST(test_refresh_config_rejects_unsafe_one_minute_combinations);
    RUN_TEST(test_offline_partial_policy_enforces_both_caps);

    int failures = UNITY_END();
    /* Surface the result as a process exit code so the devcontainer command
     * (and any CI wrapper) fails the build when a case regresses. */
    exit(failures == 0 ? 0 : 1);
}
