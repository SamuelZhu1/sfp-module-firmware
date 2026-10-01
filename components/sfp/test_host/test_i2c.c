// Transport: retries, backoff and bus recovery against the fake bus.
#include "fake_port.h"
#include "fixtures.h"
#include "sfp_internal.h"
#include "sfp_port.h"
#include "unity.h"

void setUp(void) {
    fake_reset();
    fake_load(fx_lr_a0, fx_lr_a2);
    fake_insert();
    sfp_core_init(NULL);  // also clears transport stats
    sfp_port_set_power(true);
    sfp_i2c_clear_stats();
}
void tearDown(void) {}

static void test_clean_read(void) {
    uint8_t b[96];
    TEST_ASSERT_EQUAL(SFP_OK, sfp_i2c_read(SFP_A0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_MEMORY(fx_lr_a0, b, 96);
    TEST_ASSERT_EQUAL_UINT32(1, sfp_i2c_stats()->transfers);
    TEST_ASSERT_EQUAL_UINT32(0, sfp_i2c_stats()->retries);
}

static void test_nack_then_success_on_retry(void) {
    fake_nack_next(2);
    uint8_t b[4];
    TEST_ASSERT_EQUAL(SFP_OK, sfp_i2c_read(SFP_A0, 20, b, sizeof b));
    TEST_ASSERT_EQUAL_UINT32(3, sfp_i2c_stats()->transfers);
    TEST_ASSERT_EQUAL_UINT32(2, sfp_i2c_stats()->nacks);
    TEST_ASSERT_EQUAL_UINT32(2, sfp_i2c_stats()->retries);
}

static void test_gives_up_after_four_attempts_with_backoff(void) {
    fake_nack_next(10);
    uint8_t b[4];
    uint64_t t0 = fake_now_us();
    TEST_ASSERT_EQUAL(SFP_ERR_NACK, sfp_i2c_read(SFP_A0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_UINT32(4, sfp_i2c_stats()->transfers);
    // 2 + 4 + 8 ms of backoff, plus wire time
    uint64_t waited = fake_now_us() - t0;
    TEST_ASSERT_TRUE(waited >= 14000 && waited < 18000);
}

static void test_stuck_sda_recovered_by_clocking(void) {
    fake_stuck_sda(5);  // slave needs 5 more clocks to finish its byte
    uint8_t b[96];
    TEST_ASSERT_EQUAL(SFP_OK, sfp_i2c_read(SFP_A0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_UINT32(1, sfp_i2c_stats()->bus_recoveries);
    TEST_ASSERT_EQUAL_MEMORY(fx_lr_a0, b, 96);
}

static void test_stuck_beyond_nine_clocks_is_a_bus_error(void) {
    fake_stuck_sda(1000);
    uint8_t b[4];
    TEST_ASSERT_EQUAL(SFP_ERR_BUS, sfp_i2c_read(SFP_A0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_UINT32(4, sfp_i2c_stats()->bus_recoveries);
}

static void test_sda_held_by_gpio_recovered_by_retry_backoff(void) {
    // a GPIO hold doesn't release on clocks, only with time: 5 ms is
    // outlasted by the 2 + 4 ms of backoff
    sfp_port_hold_sda_low(5);
    uint8_t b[4];
    TEST_ASSERT_EQUAL(SFP_OK, sfp_i2c_read(SFP_A0, 20, b, sizeof b));
    TEST_ASSERT_TRUE(sfp_i2c_stats()->retries >= 1);
}

static void test_injected_nack_counts(void) {
    sfp_i2c_inject_nack(3);
    uint8_t b[4];
    TEST_ASSERT_EQUAL(SFP_OK, sfp_i2c_read(SFP_A0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_UINT32(3, sfp_i2c_stats()->nacks);
}

static void test_bad_args(void) {
    uint8_t b[4];
    TEST_ASSERT_EQUAL(SFP_ERR_ARG, sfp_i2c_read(SFP_A0, 0, NULL, 4));
    TEST_ASSERT_EQUAL(SFP_ERR_ARG, sfp_i2c_read(SFP_A0, 0, b, 0));
    TEST_ASSERT_EQUAL(SFP_ERR_ARG, sfp_i2c_read(SFP_A0, 250, b, 10));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_read);
    RUN_TEST(test_nack_then_success_on_retry);
    RUN_TEST(test_gives_up_after_four_attempts_with_backoff);
    RUN_TEST(test_stuck_sda_recovered_by_clocking);
    RUN_TEST(test_stuck_beyond_nine_clocks_is_a_bus_error);
    RUN_TEST(test_sda_held_by_gpio_recovered_by_retry_backoff);
    RUN_TEST(test_injected_nack_counts);
    RUN_TEST(test_bad_args);
    return UNITY_END();
}
