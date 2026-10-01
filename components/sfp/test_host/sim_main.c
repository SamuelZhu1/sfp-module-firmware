// Simulator: the real SDK and shell running against the fake module, read
// from stdin and answered on stdout, one line each way. The pytest suite in
// tests_hil drives this in CI exactly the way it drives the board over USB.
//
// Extra "sim" commands stand in for the bench:
//   sim wait <ms>          let fake time pass (the suite's sleep)
//   sim insert | remove    plug or pull the module
//   sim los on|off         far-end transmitter off / on
//   sim txfault <n>        TX_FAULT that clears after n resets (0 = never)
//   sim stuck <clocks>     slave holds SDA until it sees this many clocks
//   sim module lr|sr_ext|noddm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_port.h"
#include "fixtures.h"
#include "sfp_internal.h"
#include "sfp_shell.h"

static void load(const char *name) {
    if (!strcmp(name, "sr_ext")) fake_load(fx_sr_ext_a0, fx_sr_ext_a2);
    else if (!strcmp(name, "noddm")) fake_load(fx_noddm_a0, fx_noddm_a2);
    else fake_load(fx_lr_a0, fx_lr_a2);
}

static int sim_cmd(char *args, char *out, size_t len) {
    char *save = NULL;
    char *cmd = strtok_r(args, " \t\r\n", &save);
    char *arg = strtok_r(NULL, " \t\r\n", &save);
    if (!cmd) return snprintf(out, len, "{\"error\":\"sim\"}");
    if (!strcmp(cmd, "wait") && arg) {
        fake_run_for((uint32_t)strtoul(arg, NULL, 0));
    } else if (!strcmp(cmd, "insert")) {
        fake_insert();
    } else if (!strcmp(cmd, "remove")) {
        fake_remove();
    } else if (!strcmp(cmd, "los") && arg) {
        fake_set_los(!strcmp(arg, "on"));
    } else if (!strcmp(cmd, "txfault") && arg) {
        fake_tx_fault((uint32_t)strtoul(arg, NULL, 0));
    } else if (!strcmp(cmd, "stuck") && arg) {
        fake_stuck_sda((uint32_t)strtoul(arg, NULL, 0));
    } else if (!strcmp(cmd, "module") && arg) {
        load(arg);
    } else if (!strcmp(cmd, "now")) {
        return snprintf(out, len, "{\"t_us\":%llu}", (unsigned long long)fake_now_us());
    } else {
        return snprintf(out, len, "{\"error\":\"sim\"}");
    }
    return snprintf(out, len, "{\"ok\":true}");
}

int main(int argc, char **argv) {
    fake_reset();
    load(argc > 1 ? argv[1] : "lr");
    sfp_core_init(NULL);
    char line[256], out[1024];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "sim ", 4)) {
            sim_cmd(line + 4, out, sizeof out);
        } else {
            fake_run_for(1);  // a shell round trip over USB takes about a millisecond
            sfp_shell_handle(line, out, sizeof out);
        }
        printf("%s\n", out);
    }
    return 0;
}
