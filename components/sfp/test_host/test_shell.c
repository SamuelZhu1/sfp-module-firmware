// Shell replies: one JSON object per command.
#include <string.h>

#include "fake_port.h"
#include "fixtures.h"
#include "sfp_internal.h"
#include "sfp_shell.h"
#include "unity.h"

static char out[512];

static const char *run(const char *line) {
    sfp_shell_handle(line, out, sizeof out);
    return out;
}

void setUp(void) {
    fake_reset();
    fake_load(fx_lr_a0, fx_lr_a2);
    sfp_core_init(NULL);
}
void tearDown(void) {}

static void ready(void) {
    fake_insert();
    fake_run_until_state(SFP_READY, 1000);
    fake_run_for(100);
}

static void test_state_absent(void) { TEST_ASSERT_EQUAL_STRING("{\"state\":\"ABSENT\"}", run("state")); }

static void test_id_before_insert_is_an_error(void) {
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"ABSENT\"}", run("id"));
}

static void test_id_json(void) {
    ready();
    TEST_ASSERT_EQUAL_STRING(
        "{\"vendor\":\"FINISAR CORP.\",\"part\":\"FTLX1471D3BCL\",\"serial\":\"UZ80TQ2\","
        "\"wavelength_nm\":1310,\"bitrate_mbps\":10300,\"ddm\":true,\"ext_cal\":false}",
        run("id"));
}

static void test_ddm_json(void) {
    ready();
    run("ddm");
    TEST_ASSERT_NOT_NULL(strstr(out, "\"temp_c\":35.50"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"vcc_v\":3.3000"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"bias_ma\":6.800"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"tx_dbm\":-2.4"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"los\":false"));
}

static void test_ddm_no_light_is_null(void) {
    ready();
    fake_set_los(true);
    run("ddm");
    TEST_ASSERT_NOT_NULL(strstr(out, "\"rx_dbm\":null"));
}

static void test_raw_hex(void) {
    ready();
    TEST_ASSERT_EQUAL_STRING("{\"hex\":\"030407\"}", run("raw a0 0 3"));
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"ARG\"}", run("raw a0 250 10"));
}

static void test_power_and_txdis(void) {
    ready();
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", run("power off"));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"OFF\"}", run("state"));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", run("txdis on"));
    TEST_ASSERT_TRUE(fake_tx_disabled());
}

static void test_alarms_json(void) {
    ready();
    fake_set_alarm_bytes(0x80, 0x40);
    fake_run_for(100);
    TEST_ASSERT_EQUAL_STRING("{\"alarms\":[\"temp_high\",\"rx_low\"],\"warnings\":[]}",
                             run("alarms"));
}

static void test_inject_and_stats(void) {
    ready();
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", run("inject nack 2"));
    fake_run_for(100);
    run("stats");
    TEST_ASSERT_NOT_NULL(strstr(out, "\"i2c_nack\":2"));
}

static void test_unknown_command(void) {
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"unknown command\"}", run("reboot now"));
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"unknown command\"}", run("power maybe"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_state_absent);
    RUN_TEST(test_id_before_insert_is_an_error);
    RUN_TEST(test_id_json);
    RUN_TEST(test_ddm_json);
    RUN_TEST(test_ddm_no_light_is_null);
    RUN_TEST(test_raw_hex);
    RUN_TEST(test_power_and_txdis);
    RUN_TEST(test_alarms_json);
    RUN_TEST(test_inject_and_stats);
    RUN_TEST(test_unknown_command);
    return UNITY_END();
}
