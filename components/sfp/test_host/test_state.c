// Module state machine: insertion, removal, identification retries, fault
// recovery, loss of signal, bus failure handling. Runs on a fake clock.
#include "fake_port.h"
#include "fixtures.h"
#include "sfp_internal.h"
#include "unity.h"

static sfp_state_t state(void) {
    sfp_state_t s;
    sfp_get_state(&s);
    return s;
}

void setUp(void) {
    fake_reset();
    fake_load(fx_lr_a0, fx_lr_a2);
    sfp_core_init(NULL);
}
void tearDown(void) {}

static void test_insert_reaches_ready_after_t_init(void) {
    TEST_ASSERT_EQUAL(SFP_ABSENT, state());
    fake_insert();
    uint32_t ms = fake_run_until_state(SFP_READY, 1000);
    // 20 ms debounce + 300 ms t_init + A0h and A2h reads at 100 kHz
    TEST_ASSERT_TRUE(ms >= 320 && ms < 345);
    sfp_stats_t s;
    sfp_get_stats(&s);
    // last_ready_ms is stamped at identification; the first DDM read that
    // follows adds about 2 ms of bus time before the loop sees READY
    TEST_ASSERT_UINT32_WITHIN(3, ms, s.last_ready_ms);
    sfp_ddm_t d;
    TEST_ASSERT_EQUAL(SFP_OK, sfp_get_ddm(&d));  // data the moment READY shows
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_OK, sfp_get_identity(&id));
    TEST_ASSERT_EQUAL_STRING("FTLX1471D3BCL", id.part);
}

static void test_bounce_shorter_than_debounce_is_ignored(void) {
    fake_insert();
    fake_run_for(5);
    fake_remove();
    fake_run_for(500);
    TEST_ASSERT_EQUAL(SFP_ABSENT, state());
    TEST_ASSERT_FALSE(fake_powered());
}

static void test_removal_while_powering_stops_everything(void) {
    fake_insert();
    fake_run_for(100);  // powered, still inside t_init
    TEST_ASSERT_EQUAL(SFP_POWERING, state());
    uint32_t before = fake_transactions();
    fake_remove();
    fake_run_for(1000);
    TEST_ASSERT_EQUAL(SFP_ABSENT, state());
    TEST_ASSERT_FALSE(fake_powered());
    TEST_ASSERT_EQUAL_UINT32(before, fake_transactions());  // no bus traffic to an empty cage
}

static void test_ddm_polled_at_10_hz(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    sfp_stats_t s0, s1;
    sfp_get_stats(&s0);
    fake_run_for(1000);
    sfp_get_stats(&s1);
    TEST_ASSERT_EQUAL_UINT32(10, s1.polls - s0.polls);
    fake_set_live(30 * 256, 33100, 3500, 6000, 2000);
    fake_run_for(100);
    sfp_ddm_t d;
    TEST_ASSERT_EQUAL(SFP_OK, sfp_get_ddm(&d));
    TEST_ASSERT_EQUAL_UINT16(2000, d.rx_01uw);
    TEST_ASSERT_TRUE(d.age_ms <= 100);
}

static void test_corrupted_identity_read_retried(void) {
    fake_corrupt_next(1);
    fake_insert();
    TEST_ASSERT_TRUE(fake_run_until_state(SFP_READY, 1000) < 1000);
    sfp_stats_t s;
    sfp_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.checksum_fail);
}

static void test_bad_checksum_every_time_is_invalid(void) {
    uint8_t a0[256];
    for (int i = 0; i < 256; ++i) a0[i] = fx_lr_a0[i];
    a0[63] ^= 0xFF;
    fake_load(a0, fx_lr_a2);
    fake_insert();
    fake_run_for(1000);
    TEST_ASSERT_EQUAL(SFP_INVALID, state());
}

static void test_module_slow_to_answer_after_power_on(void) {
    // some modules answer late; NACKs during the first reads are retried
    fake_set_module_boot_ms(305);
    fake_insert();
    TEST_ASSERT_TRUE(fake_run_until_state(SFP_READY, 1000) < 400);
}

static void test_tx_fault_recovered_on_second_attempt(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_tx_fault(2);  // clears after the second TX_DISABLE pulse
    fake_run_for(1000);
    TEST_ASSERT_EQUAL(SFP_READY, state());
    TEST_ASSERT_FALSE(fake_tx_disabled());
    sfp_stats_t s;
    sfp_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(2, s.fault_recoveries);
}

static void test_tx_fault_latched_after_three_attempts(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_tx_fault(0);  // never clears
    fake_run_for(2000);
    TEST_ASSERT_EQUAL(SFP_TX_FAULT, state());
    TEST_ASSERT_TRUE(fake_tx_disabled());  // laser kept off
    sfp_set_tx_disable(false);
    TEST_ASSERT_TRUE(fake_tx_disabled());  // and stays off
}

static void test_loss_of_signal_reports_no_light(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_run_for(100);
    fake_set_los(true);
    sfp_ddm_t d;
    sfp_get_ddm(&d);
    TEST_ASSERT_TRUE(d.los);
    TEST_ASSERT_EQUAL_INT16(SFP_NO_LIGHT, d.rx_cdbm);
    // clears only once RX_LOS has stayed low for 50 ms
    fake_set_los(false);
    fake_run_for(20);
    sfp_get_ddm(&d);
    TEST_ASSERT_TRUE(d.los);
    fake_run_for(40);
    sfp_get_ddm(&d);
    TEST_ASSERT_FALSE(d.los);
}

static void test_poll_failures_power_cycle_then_recover(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_nack_next(12);  // 3 polls x 4 attempts
    fake_run_for(2000);
    TEST_ASSERT_EQUAL(SFP_READY, state());
    sfp_stats_t s;
    sfp_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(1, s.power_cycles);
}

static void test_dead_bus_after_power_cycle_is_bus_error(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_stuck_sda(1000000);
    fake_run_for(3000);
    TEST_ASSERT_EQUAL(SFP_BUS_ERROR, state());
    TEST_ASSERT_FALSE(fake_powered());
}

static void test_power_off_and_on(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    TEST_ASSERT_EQUAL(SFP_OK, sfp_power(false));
    TEST_ASSERT_EQUAL(SFP_OFF, state());
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_ERR_NOT_READY, sfp_get_identity(&id));
    TEST_ASSERT_EQUAL(SFP_OK, sfp_power(true));
    uint32_t ms = fake_run_until_state(SFP_READY, 1000);
    TEST_ASSERT_TRUE(ms >= 300 && ms < 325);  // no debounce on a commanded power-on
}

static void test_no_ddm_module(void) {
    fake_load(fx_noddm_a0, fx_noddm_a2);
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_run_for(500);
    sfp_ddm_t d;
    TEST_ASSERT_EQUAL(SFP_ERR_NOT_READY, sfp_get_ddm(&d));
}

static void test_alarm_change_is_logged(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    sfp_event_t ev[64];
    sfp_events_read(ev, 64);  // drain
    fake_set_alarm_bytes(0x00, 0x40);  // rx power low
    fake_run_for(100);
    size_t n = sfp_events_read(ev, 64);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL(SFP_EV_ALARM, ev[0].type);
    TEST_ASSERT_EQUAL_HEX32(SFP_ALM_RX_LOW, ev[0].arg);
}

static void test_event_log_order(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    sfp_event_t ev[8];
    size_t n = sfp_events_read(ev, 8);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL(SFP_EV_MOD_PRESENT, ev[0].type);
    TEST_ASSERT_EQUAL_UINT32(SFP_POWERING, ev[1].arg);
    TEST_ASSERT_EQUAL_UINT32(SFP_IDENTIFYING, ev[2].arg);
    TEST_ASSERT_EQUAL_UINT32(SFP_READY, ev[3].arg);
    TEST_ASSERT_TRUE(ev[0].t_us < ev[3].t_us);
}

static void test_raw_read_absent(void) {
    uint8_t b[4];
    TEST_ASSERT_EQUAL(SFP_ERR_ABSENT, sfp_read_raw(0x50, 0, b, 4));
    TEST_ASSERT_EQUAL(SFP_ERR_ARG, sfp_read_raw(0x52, 0, b, 4));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_insert_reaches_ready_after_t_init);
    RUN_TEST(test_bounce_shorter_than_debounce_is_ignored);
    RUN_TEST(test_removal_while_powering_stops_everything);
    RUN_TEST(test_ddm_polled_at_10_hz);
    RUN_TEST(test_corrupted_identity_read_retried);
    RUN_TEST(test_bad_checksum_every_time_is_invalid);
    RUN_TEST(test_module_slow_to_answer_after_power_on);
    RUN_TEST(test_tx_fault_recovered_on_second_attempt);
    RUN_TEST(test_tx_fault_latched_after_three_attempts);
    RUN_TEST(test_loss_of_signal_reports_no_light);
    RUN_TEST(test_poll_failures_power_cycle_then_recover);
    RUN_TEST(test_dead_bus_after_power_cycle_is_bus_error);
    RUN_TEST(test_power_off_and_on);
    RUN_TEST(test_no_ddm_module);
    RUN_TEST(test_alarm_change_is_logged);
    RUN_TEST(test_event_log_order);
    RUN_TEST(test_raw_read_absent);
    return UNITY_END();
}
