// Fault campaign on the fake bus while the module is being polled: 200 NACK
// bursts and 200 stuck-SDA events with seeded random sizes. Every fault must
// be recovered without leaving READY, and the counters must match what was
// injected. The same campaign runs on the board through the shell's inject
// command (tests_hil/test_fault_injection.py); these are host results only.
#include <stdio.h>

#include "fake_port.h"
#include "fixtures.h"
#include "sfp_internal.h"
#include "unity.h"

static uint32_t rng = 12345;
static uint32_t next_rand(void) {  // xorshift32, fixed seed so failures replay
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void setUp(void) {
    fake_reset();
    fake_load(fx_lr_a0, fx_lr_a2);
    sfp_core_init(NULL);
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    sfp_clear_stats();
}
void tearDown(void) {}

static void test_nack_bursts(void) {
    uint32_t injected = 0, recovered = 0;
    for (int i = 0; i < 200; ++i) {
        uint32_t burst = 1 + next_rand() % 3;  // within the 3 retries
        injected += burst;
        fake_nack_next(burst);
        sfp_stats_t before;
        sfp_get_stats(&before);
        fake_run_for(100);  // one poll
        sfp_state_t s;
        sfp_get_state(&s);
        sfp_ddm_t d;
        if (s == SFP_READY && sfp_get_ddm(&d) == SFP_OK && d.age_ms < 100) ++recovered;
    }
    sfp_stats_t st;
    sfp_get_stats(&st);
    printf("NACK bursts: %u of 200 recovered, %u NACKs injected, %u counted\n",
           (unsigned)recovered, (unsigned)injected, (unsigned)st.i2c_nack);
    TEST_ASSERT_EQUAL_UINT32(200, recovered);
    TEST_ASSERT_EQUAL_UINT32(injected, st.i2c_nack);
    TEST_ASSERT_EQUAL_UINT32(0, st.power_cycles);
}

static void test_stuck_sda(void) {
    uint32_t recovered = 0;
    for (int i = 0; i < 200; ++i) {
        fake_stuck_sda(1 + next_rand() % 8);  // released within the 9 recovery clocks
        fake_run_for(100);
        sfp_state_t s;
        sfp_get_state(&s);
        sfp_ddm_t d;
        if (s == SFP_READY && sfp_get_ddm(&d) == SFP_OK && d.age_ms < 100) ++recovered;
    }
    sfp_stats_t st;
    sfp_get_stats(&st);
    printf("Stuck SDA: %u of 200 recovered, %u bus recoveries\n", (unsigned)recovered,
           (unsigned)st.bus_recoveries);
    TEST_ASSERT_EQUAL_UINT32(200, recovered);
    TEST_ASSERT_EQUAL_UINT32(200, st.bus_recoveries);
}

// The controller on the RISC-V core read zeros from a stuck bus instead of
// failing. If the port ever does that, the identity check has to catch it.
static void test_stuck_bus_reading_zeros_never_identifies(void) {
    sfp_power(false);
    fake_stuck_sda_returns_zeros(true);
    fake_stuck_sda(1000000);
    sfp_power(true);
    fake_run_for(2000);
    sfp_state_t s;
    sfp_get_state(&s);
    TEST_ASSERT_NOT_EQUAL(SFP_READY, s);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nack_bursts);
    RUN_TEST(test_stuck_sda);
    RUN_TEST(test_stuck_bus_reading_zeros_never_identifies);
    return UNITY_END();
}
