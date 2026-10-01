// SFF-8472 decoding against EEPROM fixtures.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixtures.h"
#include "sfp_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_identity_fields(void) {
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_OK, sfp_decode_identity(fx_lr_a0, &id));
    TEST_ASSERT_EQUAL_STRING("FINISAR CORP.", id.vendor);
    TEST_ASSERT_EQUAL_STRING("FTLX1471D3BCL", id.part);
    TEST_ASSERT_EQUAL_STRING("UZ80TQ2", id.serial);
    TEST_ASSERT_EQUAL_UINT16(1310, id.wavelength_nm);
    TEST_ASSERT_EQUAL_UINT8(0x67, id.bitrate_100mbps);
    TEST_ASSERT_EQUAL_UINT8(0x20, id.compliance_10g);  // 10GBASE-LR
    TEST_ASSERT_TRUE(id.ddm);
    TEST_ASSERT_FALSE(id.ext_cal);
    TEST_ASSERT_FALSE(id.addr_change);
}

static void test_cc_base_mismatch_rejected(void) {
    uint8_t a0[96];
    memcpy(a0, fx_lr_a0, 96);
    a0[30] ^= 0x01;  // inside the CC_BASE range
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_ERR_CHECKSUM, sfp_decode_identity(a0, &id));
}

static void test_cc_ext_mismatch_rejected(void) {
    uint8_t a0[96];
    memcpy(a0, fx_lr_a0, 96);
    a0[70] ^= 0x01;  // inside the CC_EXT range
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_ERR_CHECKSUM, sfp_decode_identity(a0, &id));
}

// All zeros is what a stuck bus reads back as. Both checksums pass on it
// (0 == 0), so the identifier check is what has to catch it.
static void test_all_zeros_rejected_even_though_checksums_pass(void) {
    uint8_t a0[96] = {0};
    TEST_ASSERT_EQUAL_UINT8(a0[63], sfp_sum8(a0, 63));
    TEST_ASSERT_EQUAL_UINT8(a0[95], sfp_sum8(a0 + 64, 31));
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_ERR_CHECKSUM, sfp_decode_identity(a0, &id));
}

static void test_non_sfp_identifier_rejected(void) {
    uint8_t a0[96];
    memcpy(a0, fx_lr_a0, 96);
    a0[0] = 0x0D;  // QSFP+
    a0[63] = sfp_sum8(a0, 63);
    sfp_identity_t id;
    TEST_ASSERT_EQUAL(SFP_ERR_CHECKSUM, sfp_decode_identity(a0, &id));
}

static void test_dbm_matches_float_across_range(void) {
    // 0.1 uW to 6.5535 mW, every value: worst error must stay under 0.02 dB
    int worst = 0;
    for (uint32_t p = 1; p <= 65535; ++p) {
        int16_t got = sfp_power_to_cdbm((uint16_t)p);
        int want = (int)lround(1000.0 * log10(p / 10000.0));
        int err = abs(got - want);
        if (err > worst) worst = err;
    }
    printf("dBm conversion worst error: 0.%02d dB\n", worst);
    TEST_ASSERT_LESS_OR_EQUAL_INT(2, worst);
}

static void test_dbm_reference_points(void) {
    TEST_ASSERT_INT16_WITHIN(1, 0, sfp_power_to_cdbm(10000));     // 1 mW = 0 dBm
    TEST_ASSERT_INT16_WITHIN(1, -1000, sfp_power_to_cdbm(1000));  // 0.1 mW
    TEST_ASSERT_INT16_WITHIN(1, -4000, sfp_power_to_cdbm(1));     // 0.1 uW
    TEST_ASSERT_EQUAL_INT16(SFP_NO_LIGHT, sfp_power_to_cdbm(0));
}

static void test_thresholds(void) {
    sfp_thresholds_t th;
    sfp_decode_thresholds(fx_lr_a2, &th);
    TEST_ASSERT_EQUAL_INT32(78 * 256, th.temp.high_alarm);
    TEST_ASSERT_EQUAL_INT32(-13 * 256, th.temp.low_alarm);  // signed
    TEST_ASSERT_EQUAL_INT32(29000, th.vcc.low_alarm);
    TEST_ASSERT_EQUAL_INT32(6600, th.bias.high_alarm);
    TEST_ASSERT_EQUAL_INT32(1905, th.tx_power.low_warn);
    TEST_ASSERT_EQUAL_INT32(100, th.rx_power.low_alarm);
}

static void test_live_values_internal_cal(void) {
    sfp_ddm_t d;
    uint32_t alarms;
    uint8_t status;
    sfp_decode_ddm(fx_lr_a2 + 96, NULL, &d, &alarms, &status);
    TEST_ASSERT_EQUAL_INT16((int16_t)(35.5 * 256), d.temp_c256);
    TEST_ASSERT_EQUAL_UINT16(33000, d.vcc_100uv);
    TEST_ASSERT_EQUAL_UINT16(3400, d.bias_2ua);
    TEST_ASSERT_INT16_WITHIN(1, -240, d.tx_cdbm);  // 0.5754 mW
    TEST_ASSERT_INT16_WITHIN(1, -510, d.rx_cdbm);  // 0.3090 mW
    TEST_ASSERT_EQUAL_UINT32(0, alarms);
}

static void test_live_values_external_cal(void) {
    sfp_ext_cal_t cal;
    sfp_decode_ext_cal(fx_sr_ext_a2 + 56, &cal);
    sfp_ddm_t d;
    sfp_decode_ddm(fx_sr_ext_a2 + 96, &cal, &d, NULL, NULL);
    TEST_ASSERT_EQUAL_INT16((int16_t)(39 * 256), d.temp_c256);  // 40 C raw, -1 C offset
    TEST_ASSERT_EQUAL_UINT16(6000, d.tx_01uw);                  // 4000 * 1.5
    TEST_ASSERT_EQUAL_UINT16(2010, d.rx_01uw);                  // 2 * 1000 + 10
}

static void test_alarm_and_warning_flags(void) {
    uint8_t live[22];
    memcpy(live, fx_lr_a2 + 96, 22);
    live[16] = 0x80;  // 112: temp high alarm
    live[17] = 0x40;  // 113: rx power low alarm
    live[21] = 0x40;  // 117: rx power low warning
    sfp_ddm_t d;
    uint32_t f;
    sfp_decode_ddm(live, NULL, &d, &f, NULL);
    TEST_ASSERT_EQUAL_HEX32(SFP_ALM_TEMP_HIGH | SFP_ALM_RX_LOW | (SFP_ALM_RX_LOW << 16), f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identity_fields);
    RUN_TEST(test_cc_base_mismatch_rejected);
    RUN_TEST(test_cc_ext_mismatch_rejected);
    RUN_TEST(test_all_zeros_rejected_even_though_checksums_pass);
    RUN_TEST(test_non_sfp_identifier_rejected);
    RUN_TEST(test_dbm_matches_float_across_range);
    RUN_TEST(test_dbm_reference_points);
    RUN_TEST(test_thresholds);
    RUN_TEST(test_live_values_internal_cal);
    RUN_TEST(test_live_values_external_cal);
    RUN_TEST(test_alarm_and_warning_flags);
    return UNITY_END();
}
