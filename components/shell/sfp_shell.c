#include "sfp_shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sfp.h"

#ifdef SFP_TEST_HOOKS
#include "sfp_test_hooks.h"
#endif

typedef struct {
    char *buf;
    size_t len, cap;
} out_t;

static void put(out_t *o, const char *fmt, ...) {
    if (o->len >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
}

static void err_reply(out_t *o, sfp_err_t e) { put(o, "{\"error\":\"%s\"}", sfp_err_name(e)); }

// hundredths of a dBm as JSON: a number, or null for "no light"
static void put_cdbm(out_t *o, const char *key, int16_t cdbm) {
    if (cdbm == SFP_NO_LIGHT) {
        put(o, "\"%s\":null", key);
    } else {
        int v = cdbm < 0 ? -cdbm : cdbm;
        put(o, "\"%s\":%s%d.%02d", key, cdbm < 0 ? "-" : "", v / 100, v % 100);
    }
}

static void put_limits(out_t *o, const char *key, const sfp_limits_t *l, int last) {
    put(o, "\"%s\":[%ld,%ld,%ld,%ld]%s", key, (long)l->high_alarm, (long)l->low_alarm,
        (long)l->high_warn, (long)l->low_warn, last ? "" : ",");
}

static const char *event_name(sfp_event_type_t t) {
    switch (t) {
    case SFP_EV_MOD_PRESENT: return "MOD_PRESENT";
    case SFP_EV_MOD_REMOVED: return "MOD_REMOVED";
    case SFP_EV_LOS_ASSERT: return "LOS";
    case SFP_EV_LOS_CLEAR: return "LOS_CLEAR";
    case SFP_EV_TX_FAULT: return "TX_FAULT";
    case SFP_EV_POLL: return "POLL";
    case SFP_EV_STATE: return "STATE";
    case SFP_EV_FAULT_RECOVERY: return "FAULT_RECOVERY";
    case SFP_EV_BUS_RECOVERY: return "BUS_RECOVERY";
    case SFP_EV_ALARM: return "ALARM";
    }
    return "?";
}

// bit 6 + index: SFF-8472 bytes 112-113 / 116-117 order
static const char *alarm_names[10] = {"rx_low",   "rx_high",   "tx_low",  "tx_high",
                                      "bias_low", "bias_high", "vcc_low", "vcc_high",
                                      "temp_low", "temp_high"};

static void put_flag_list(out_t *o, uint32_t bits) {
    int first = 1;
    put(o, "[");
    for (int b = 15; b >= 6; --b) {
        if (bits & (1u << b)) {
            put(o, "%s\"%s\"", first ? "" : ",", alarm_names[b - 6]);
            first = 0;
        }
    }
    put(o, "]");
}

static int parse_on_off(const char *s, int *on) {
    if (!s) return 0;
    if (!strcmp(s, "on")) return *on = 1, 1;
    if (!strcmp(s, "off")) return *on = 0, 1;
    return 0;
}

static void cmd_state(out_t *o) {
    sfp_state_t s;
    sfp_get_state(&s);
    put(o, "{\"state\":\"%s\"}", sfp_state_name(s));
}

static void cmd_id(out_t *o) {
    sfp_identity_t id;
    sfp_err_t e = sfp_get_identity(&id);
    if (e) return err_reply(o, e);
    put(o,
        "{\"vendor\":\"%s\",\"part\":\"%s\",\"serial\":\"%s\",\"wavelength_nm\":%u,"
        "\"bitrate_mbps\":%u,\"ddm\":%s,\"ext_cal\":%s}",
        id.vendor, id.part, id.serial, id.wavelength_nm, id.bitrate_100mbps * 100u,
        id.ddm ? "true" : "false", id.ext_cal ? "true" : "false");
}

static void cmd_ddm(out_t *o) {
    sfp_ddm_t d;
    sfp_err_t e = sfp_get_ddm(&d);
    if (e) return err_reply(o, e);
    // integer formatting only: no printf %f on the target
    int t = d.temp_c256 * 100 / 256;  // hundredths of a degree
    int ta = t < 0 ? -t : t;
    put(o, "{\"temp_c\":%s%d.%02d,\"vcc_v\":%u.%04u,\"bias_ma\":%u.%03u,", t < 0 ? "-" : "",
        ta / 100, ta % 100, d.vcc_100uv / 10000, d.vcc_100uv % 10000, d.bias_2ua * 2 / 1000,
        d.bias_2ua * 2 % 1000);
    put_cdbm(o, "tx_dbm", d.tx_cdbm);
    put(o, ",");
    put_cdbm(o, "rx_dbm", d.rx_cdbm);
    put(o, ",\"los\":%s,\"age_ms\":%lu}", d.los ? "true" : "false", (unsigned long)d.age_ms);
}

static void cmd_thresholds(out_t *o) {
    sfp_thresholds_t th;
    sfp_err_t e = sfp_get_thresholds(&th);
    if (e) return err_reply(o, e);
    put(o, "{");
    put_limits(o, "temp", &th.temp, 0);
    put_limits(o, "vcc", &th.vcc, 0);
    put_limits(o, "bias", &th.bias, 0);
    put_limits(o, "tx_power", &th.tx_power, 0);
    put_limits(o, "rx_power", &th.rx_power, 1);
    put(o, "}");
}

static void cmd_alarms(out_t *o) {
    uint32_t f;
    sfp_err_t e = sfp_get_alarms(&f);
    if (e) return err_reply(o, e);
    put(o, "{\"alarms\":");
    put_flag_list(o, f & 0xFFFF);
    put(o, ",\"warnings\":");
    put_flag_list(o, f >> 16);
    put(o, "}");
}

static void cmd_raw(out_t *o, char *page, char *off_s, char *len_s) {
    if (!page || !off_s || !len_s) return err_reply(o, SFP_ERR_ARG);
    uint8_t dev = !strcmp(page, "a0") ? 0x50 : !strcmp(page, "a2") ? 0x51 : 0;
    long off = strtol(off_s, NULL, 0), n = strtol(len_s, NULL, 0);
    if (!dev || off < 0 || off > 255 || n < 1 || n > 128 || off + n > 256)
        return err_reply(o, SFP_ERR_ARG);
    uint8_t b[128];
    sfp_err_t e = sfp_read_raw(dev, (uint8_t)off, b, (size_t)n);
    if (e) return err_reply(o, e);
    put(o, "{\"hex\":\"");
    for (long i = 0; i < n; ++i) put(o, "%02x", b[i]);
    put(o, "\"}");
}

static void cmd_events(out_t *o) {
    sfp_event_t ev[16];
    size_t n = sfp_events_read(ev, 16);
    put(o, "{\"events\":[");
    for (size_t i = 0; i < n; ++i) {
        put(o, "%s{\"t_us\":%llu,\"type\":\"%s\",\"arg\":", i ? "," : "",
            (unsigned long long)ev[i].t_us, event_name(ev[i].type));
        if (ev[i].type == SFP_EV_STATE) {
            put(o, "\"%s\"}", sfp_state_name((sfp_state_t)ev[i].arg));
        } else {
            put(o, "%lu}", (unsigned long)ev[i].arg);
        }
    }
    put(o, "]}");
}

static void cmd_stats(out_t *o, const char *arg) {
    if (arg && !strcmp(arg, "clear")) {
        sfp_clear_stats();
        put(o, "{\"ok\":true}");
        return;
    }
    sfp_stats_t s;
    sfp_get_stats(&s);
    put(o,
        "{\"polls\":%lu,\"i2c_transfers\":%lu,\"i2c_nack\":%lu,\"i2c_timeout\":%lu,"
        "\"i2c_retries\":%lu,\"bus_recoveries\":%lu,\"checksum_fail\":%lu,\"los_events\":%lu,"
        "\"fault_events\":%lu,\"fault_recoveries\":%lu,\"power_cycles\":%lu,"
        "\"events_dropped\":%lu,\"last_ready_ms\":%lu,\"worst_ready_ms\":%lu}",
        (unsigned long)s.polls, (unsigned long)s.i2c_transfers, (unsigned long)s.i2c_nack,
        (unsigned long)s.i2c_timeout, (unsigned long)s.i2c_retries,
        (unsigned long)s.bus_recoveries, (unsigned long)s.checksum_fail,
        (unsigned long)s.los_events, (unsigned long)s.fault_events,
        (unsigned long)s.fault_recoveries, (unsigned long)s.power_cycles,
        (unsigned long)s.events_dropped, (unsigned long)s.last_ready_ms,
        (unsigned long)s.worst_ready_ms);
}

size_t sfp_shell_handle(const char *line, char *out, size_t out_len) {
    out_t o = {out, 0, out_len};
    if (out_len) out[0] = 0;
    char buf[96];
    strncpy(buf, line, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *save = NULL;
    char *cmd = strtok_r(buf, " \t\r\n", &save);
    char *a1 = strtok_r(NULL, " \t\r\n", &save);
    char *a2 = strtok_r(NULL, " \t\r\n", &save);
    char *a3 = strtok_r(NULL, " \t\r\n", &save);
    int on;

    if (!cmd) {
        put(&o, "{\"error\":\"empty\"}");
    } else if (!strcmp(cmd, "state")) {
        cmd_state(&o);
    } else if (!strcmp(cmd, "id")) {
        cmd_id(&o);
    } else if (!strcmp(cmd, "ddm")) {
        cmd_ddm(&o);
    } else if (!strcmp(cmd, "thresholds")) {
        cmd_thresholds(&o);
    } else if (!strcmp(cmd, "alarms")) {
        cmd_alarms(&o);
    } else if (!strcmp(cmd, "txdis") && parse_on_off(a1, &on)) {
        sfp_set_tx_disable(on);
        put(&o, "{\"ok\":true}");
    } else if (!strcmp(cmd, "power") && parse_on_off(a1, &on)) {
        sfp_err_t e = sfp_power(on);
        if (e) err_reply(&o, e);
        else put(&o, "{\"ok\":true}");
    } else if (!strcmp(cmd, "raw")) {
        cmd_raw(&o, a1, a2, a3);
    } else if (!strcmp(cmd, "events")) {
        cmd_events(&o);
    } else if (!strcmp(cmd, "stats")) {
        cmd_stats(&o, a1);
    } else if (!strcmp(cmd, "version")) {
        put(&o, "{\"sdk\":\"%s\"}", SFP_SDK_VERSION);
#ifdef SFP_TEST_HOOKS
    } else if (!strcmp(cmd, "inject") && a1 && a2 && !strcmp(a1, "nack")) {
        sfp_test_inject_nack((uint32_t)strtoul(a2, NULL, 0));
        put(&o, "{\"ok\":true}");
    } else if (!strcmp(cmd, "inject") && a1 && a2 && !strcmp(a1, "sda_low")) {
        sfp_test_inject_sda_low((uint32_t)strtoul(a2, NULL, 0));
        put(&o, "{\"ok\":true}");
#endif
    } else {
        put(&o, "{\"error\":\"unknown command\"}");
    }
    return o.len;
}
