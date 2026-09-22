// Module state machine and the SDK calls on top of it.
//
// Pin edges and poll ticks arrive as events (sfp_core_handle). Anything that
// has to wait (debounce, the module's 300 ms t_init, fault recovery, LOS
// clear) is a timer checked by sfp_core_service, so a module pulled while
// it is still powering up is handled at once instead of after a blocking
// wait. The owner task sleeps until min(next event, next deadline).
#include <string.h>

#include "sfp_internal.h"
#include "sfp_port.h"

#define LOG_SIZE 64
#define LOS_CLEAR_MS 50
#define TX_DISABLE_PULSE_US 1000  // SFF-8431 asks for >= 10 us
#define FAULT_ATTEMPTS 3
#define POLL_FAILS_BEFORE_CYCLE 3
#define POWER_OFF_SETTLE_US 100000
#define NONE UINT64_MAX

typedef struct {
    sfp_config_t cfg;
    sfp_state_t state;
    bool powered;
    bool user_tx_disable;
    bool power_cycled;        // one automatic power cycle per insertion
    bool los;
    uint8_t fault_attempt;
    uint8_t poll_fails;
    uint64_t insert_t;        // pin edge (or power on) that started this bring-up
    uint64_t debounce_at, ready_at, fault_check_at, los_clear_at;
    uint64_t ddm_t;
    sfp_identity_t id;
    sfp_thresholds_t th;
    sfp_ext_cal_t cal;
    sfp_ddm_t ddm;
    uint32_t alarms;
    sfp_stats_t stats;
    sfp_event_t log[LOG_SIZE];
    size_t log_head, log_count;
    sfp_event_cb_t cb;
    void *cb_ctx;
} core_t;

static core_t c;

static uint64_t now(void) { return sfp_port_now_us(); }

static void log_event(sfp_event_type_t type, uint32_t arg, uint64_t t) {
    sfp_event_t ev = {.t_us = t, .type = type, .arg = arg};
    size_t slot = (c.log_head + c.log_count) % LOG_SIZE;
    if (c.log_count == LOG_SIZE) {
        c.log_head = (c.log_head + 1) % LOG_SIZE;  // overwrite the oldest
    } else {
        ++c.log_count;
    }
    c.log[slot] = ev;
    if (c.cb) c.cb(&ev, c.cb_ctx);
}

static void set_state(sfp_state_t s) {
    if (c.state == s) return;
    c.state = s;
    log_event(SFP_EV_STATE, (uint32_t)s, now());
}

static void clear_module_data(void) {
    memset(&c.id, 0, sizeof c.id);
    memset(&c.th, 0, sizeof c.th);
    memset(&c.ddm, 0, sizeof c.ddm);
    c.alarms = 0;
    c.ddm_t = 0;
    c.debounce_at = c.ready_at = c.fault_check_at = c.los_clear_at = NONE;
    c.fault_attempt = 0;
    c.poll_fails = 0;
}

static void power_on_and_wait(uint64_t t) {
    sfp_port_set_power(true);
    c.powered = true;
    sfp_port_set_tx_disable(c.user_tx_disable);
    c.ready_at = t + (uint64_t)c.cfg.t_init_ms * 1000;
    set_state(SFP_POWERING);
}

static void power_off(void) {
    sfp_port_set_power(false);
    c.powered = false;
}

// Bus failed while the module is present: power-cycle it once, then give up.
static void bus_failure(void) {
    if (!c.power_cycled) {
        c.power_cycled = true;
        ++c.stats.power_cycles;
        power_off();
        sfp_port_delay_us(POWER_OFF_SETTLE_US);
        clear_module_data();
        power_on_and_wait(now());
    } else {
        power_off();
        set_state(SFP_BUS_ERROR);
    }
}

static void poll(void);

static void identify(void) {
    set_state(SFP_IDENTIFYING);
    uint8_t a0[96];
    sfp_err_t err = SFP_ERR_CHECKSUM;
    // a checksum mismatch is usually one corrupted transfer: read again
    for (int attempt = 0; attempt < 3; ++attempt) {
        err = sfp_i2c_read(SFP_A0, 0, a0, sizeof a0);
        if (err != SFP_OK) break;
        err = sfp_decode_identity(a0, &c.id);
        if (err == SFP_OK) break;
        ++c.stats.checksum_fail;
    }
    if (err == SFP_ERR_CHECKSUM) {
        set_state(SFP_INVALID);
        return;
    }
    if (err != SFP_OK) {
        bus_failure();
        return;
    }
    if (c.id.ddm && !c.id.addr_change) {
        uint8_t a2[96];
        err = sfp_i2c_read(SFP_A2, 0, a2, sizeof a2);
        if (err != SFP_OK) {
            bus_failure();
            return;
        }
        sfp_decode_thresholds(a2, &c.th);
        if (c.id.ext_cal) sfp_decode_ext_cal(a2 + 56, &c.cal);
    }
    uint32_t ms = (uint32_t)((now() - c.insert_t) / 1000);
    c.stats.last_ready_ms = ms;
    if (ms > c.stats.worst_ready_ms) c.stats.worst_ready_ms = ms;
    c.los = sfp_port_rx_los();
    set_state(SFP_READY);
    // first DDM read now, under the same lock, so no caller ever sees READY
    // with no diagnostics yet (it used to wait up to one 100 ms poll)
    poll();
}

static void poll(void) {
    if (c.state != SFP_READY || !c.id.ddm || c.id.addr_change) return;
    ++c.stats.polls;  // count real DDM reads, not timer ticks
    uint8_t live[22];
    sfp_err_t err = sfp_i2c_read(SFP_A2, 96, live, sizeof live);
    if (err != SFP_OK) {
        if (++c.poll_fails >= POLL_FAILS_BEFORE_CYCLE) bus_failure();
        return;
    }
    c.poll_fails = 0;
    uint32_t alarms;
    sfp_decode_ddm(live, c.id.ext_cal ? &c.cal : NULL, &c.ddm, &alarms, NULL);
    c.ddm_t = now();
    if (alarms != c.alarms) {
        log_event(SFP_EV_ALARM, alarms ^ c.alarms, c.ddm_t);
        c.alarms = alarms;
    }
}

static void start_fault_recovery(uint64_t t) {
    ++c.fault_attempt;
    ++c.stats.fault_recoveries;
    log_event(SFP_EV_FAULT_RECOVERY, c.fault_attempt, t);
    // SFF-8431 reset: assert TX_DISABLE, release it, wait t_init, look again
    sfp_port_set_tx_disable(true);
    sfp_port_delay_us(TX_DISABLE_PULSE_US);
    sfp_port_set_tx_disable(c.user_tx_disable);
    c.fault_check_at = now() + (uint64_t)c.cfg.t_init_ms * 1000;
}

static bool module_present_state(void) {
    return c.state == SFP_POWERING || c.state == SFP_IDENTIFYING || c.state == SFP_READY ||
           c.state == SFP_TX_FAULT || c.state == SFP_INVALID;
}

static void handle_locked(const sfp_event_t *ev) {
    switch (ev->type) {
    case SFP_EV_MOD_PRESENT:
        log_event(ev->type, 0, ev->t_us);
        if (c.state == SFP_ABSENT || c.state == SFP_BUS_ERROR) {
            c.insert_t = ev->t_us;
            c.power_cycled = false;
            c.debounce_at = ev->t_us + (uint64_t)c.cfg.debounce_ms * 1000;
        }
        break;
    case SFP_EV_MOD_REMOVED:
        log_event(ev->type, 0, ev->t_us);
        // stop at once: no more bus traffic to a module that isn't there
        power_off();
        clear_module_data();
        c.los = false;
        set_state(SFP_ABSENT);
        break;
    case SFP_EV_LOS_ASSERT:
        log_event(ev->type, 0, ev->t_us);
        ++c.stats.los_events;
        c.los = true;
        c.los_clear_at = NONE;
        break;
    case SFP_EV_LOS_CLEAR:
        log_event(ev->type, 0, ev->t_us);
        c.los_clear_at = ev->t_us + LOS_CLEAR_MS * 1000ull;  // clear only if it stays clear
        break;
    case SFP_EV_TX_FAULT:
        log_event(ev->type, 0, ev->t_us);
        ++c.stats.fault_events;
        if (c.state == SFP_READY && c.fault_check_at == NONE) {
            c.fault_attempt = 0;
            start_fault_recovery(ev->t_us);
        }
        break;
    case SFP_EV_POLL:
        poll();
        break;
    default:
        break;
    }
}

void sfp_core_handle(const sfp_event_t *ev) {
    sfp_port_lock();
    handle_locked(ev);
    sfp_port_unlock();
}

void sfp_core_service(void) {
    sfp_port_lock();
    uint64_t t = now();
    if (c.debounce_at != NONE && t >= c.debounce_at) {
        c.debounce_at = NONE;
        if (sfp_port_mod_present()) power_on_and_wait(t);  // else it was a bounce
    }
    if (c.ready_at != NONE && t >= c.ready_at) {
        c.ready_at = NONE;
        if (c.state == SFP_POWERING) identify();
    }
    if (c.fault_check_at != NONE && t >= c.fault_check_at) {
        c.fault_check_at = NONE;
        if (!sfp_port_tx_fault()) {
            c.fault_attempt = 0;  // recovered
        } else if (c.fault_attempt < FAULT_ATTEMPTS) {
            start_fault_recovery(t);
        } else {
            // give up: keep the laser off and report it
            sfp_port_set_tx_disable(true);
            set_state(SFP_TX_FAULT);
        }
    }
    if (c.los_clear_at != NONE && t >= c.los_clear_at) {
        c.los_clear_at = NONE;
        if (!sfp_port_rx_los()) c.los = false;
    }
    sfp_port_unlock();
}

uint64_t sfp_core_next_deadline_us(void) {
    uint64_t d = c.debounce_at;
    if (c.ready_at < d) d = c.ready_at;
    if (c.fault_check_at < d) d = c.fault_check_at;
    if (c.los_clear_at < d) d = c.los_clear_at;
    return d;
}

void sfp_core_init(const sfp_config_t *cfg) {
    memset(&c, 0, sizeof c);
    c.cfg = (sfp_config_t){.poll_period_ms = 100, .debounce_ms = 20, .t_init_ms = 300};
    if (cfg) {
        if (cfg->poll_period_ms) c.cfg.poll_period_ms = cfg->poll_period_ms;
        if (cfg->debounce_ms) c.cfg.debounce_ms = cfg->debounce_ms;
        if (cfg->t_init_ms) c.cfg.t_init_ms = cfg->t_init_ms;
    }
    clear_module_data();
    c.state = SFP_ABSENT;
    sfp_port_set_power(false);
    sfp_port_set_tx_disable(false);
    sfp_i2c_clear_stats();
    // a module already in the cage at boot counts as an insertion now
    if (sfp_port_mod_present()) {
        sfp_event_t ev = {.t_us = now(), .type = SFP_EV_MOD_PRESENT};
        handle_locked(&ev);
    }
}

void sfp_core_inject_nack(uint32_t n) {
    sfp_port_lock();
    sfp_i2c_inject_nack(n);
    sfp_port_unlock();
}

void sfp_core_inject_sda_low(uint32_t ms) { sfp_port_hold_sda_low(ms); }

void sfp_core_note_dropped(void) { ++c.stats.events_dropped; }

// ---------------- SDK ----------------

sfp_err_t sfp_init(const sfp_config_t *cfg) {
    sfp_port_init();
    sfp_core_init(cfg);
    sfp_port_start(&c.cfg);
    return SFP_OK;
}

sfp_err_t sfp_get_state(sfp_state_t *state) {
    if (!state) return SFP_ERR_ARG;
    sfp_port_lock();
    *state = c.state;
    sfp_port_unlock();
    return SFP_OK;
}

static sfp_err_t need_ready(void) {
    if (c.state == SFP_ABSENT) return SFP_ERR_ABSENT;
    if (c.state != SFP_READY && c.state != SFP_TX_FAULT) return SFP_ERR_NOT_READY;
    return SFP_OK;
}

sfp_err_t sfp_get_identity(sfp_identity_t *id) {
    if (!id) return SFP_ERR_ARG;
    sfp_port_lock();
    sfp_err_t err = need_ready();
    if (err == SFP_OK) *id = c.id;
    sfp_port_unlock();
    return err;
}

sfp_err_t sfp_get_ddm(sfp_ddm_t *ddm) {
    if (!ddm) return SFP_ERR_ARG;
    sfp_port_lock();
    sfp_err_t err = need_ready();
    if (err == SFP_OK && c.ddm_t == 0) err = SFP_ERR_NOT_READY;  // no poll yet
    if (err == SFP_OK) {
        *ddm = c.ddm;
        ddm->los = c.los;
        // with no light the module still reports a tiny number; don't pass
        // it on as if it were a measurement
        if (c.los) ddm->rx_cdbm = SFP_NO_LIGHT;
        ddm->age_ms = (uint32_t)((now() - c.ddm_t) / 1000);
    }
    sfp_port_unlock();
    return err;
}

sfp_err_t sfp_get_thresholds(sfp_thresholds_t *th) {
    if (!th) return SFP_ERR_ARG;
    sfp_port_lock();
    sfp_err_t err = need_ready();
    if (err == SFP_OK) *th = c.th;
    sfp_port_unlock();
    return err;
}

sfp_err_t sfp_get_alarms(uint32_t *flags) {
    if (!flags) return SFP_ERR_ARG;
    sfp_port_lock();
    sfp_err_t err = need_ready();
    if (err == SFP_OK) *flags = c.alarms;
    sfp_port_unlock();
    return err;
}

sfp_err_t sfp_set_tx_disable(bool disable) {
    sfp_port_lock();
    c.user_tx_disable = disable;
    // a latched fault keeps the laser off whatever the user asks
    sfp_port_set_tx_disable(disable || c.state == SFP_TX_FAULT);
    sfp_port_unlock();
    return SFP_OK;
}

sfp_err_t sfp_power(bool on) {
    sfp_port_lock();
    sfp_err_t err = SFP_OK;
    if (!sfp_port_mod_present()) {
        err = SFP_ERR_ABSENT;
    } else if (on) {
        if (!c.powered) {
            clear_module_data();
            c.insert_t = now();
            c.power_cycled = false;
            power_on_and_wait(c.insert_t);
        }
    } else {
        power_off();
        clear_module_data();
        set_state(SFP_OFF);
    }
    sfp_port_unlock();
    return err;
}

sfp_err_t sfp_read_raw(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    if (dev != SFP_A0 && dev != SFP_A2) return SFP_ERR_ARG;
    sfp_port_lock();
    sfp_err_t err = SFP_OK;
    if (!module_present_state() || !c.powered) {
        err = c.state == SFP_ABSENT ? SFP_ERR_ABSENT : SFP_ERR_NOT_READY;
    } else {
        err = sfp_i2c_read(dev, offset, buf, n);
    }
    sfp_port_unlock();
    return err;
}

sfp_err_t sfp_register_callback(sfp_event_cb_t cb, void *ctx) {
    sfp_port_lock();
    c.cb = cb;
    c.cb_ctx = ctx;
    sfp_port_unlock();
    return SFP_OK;
}

sfp_err_t sfp_get_stats(sfp_stats_t *stats) {
    if (!stats) return SFP_ERR_ARG;
    sfp_port_lock();
    *stats = c.stats;
    const sfp_i2c_stats_t *i = sfp_i2c_stats();
    stats->i2c_transfers = i->transfers;
    stats->i2c_nack = i->nacks;
    stats->i2c_timeout = i->timeouts;
    stats->i2c_retries = i->retries;
    stats->bus_recoveries = i->bus_recoveries;
    sfp_port_unlock();
    return SFP_OK;
}

sfp_err_t sfp_clear_stats(void) {
    sfp_port_lock();
    c.stats = (sfp_stats_t){0};
    sfp_i2c_clear_stats();
    sfp_port_unlock();
    return SFP_OK;
}

size_t sfp_events_read(sfp_event_t *out, size_t max) {
    sfp_port_lock();
    size_t n = 0;
    while (n < max && c.log_count) {
        out[n++] = c.log[c.log_head];
        c.log_head = (c.log_head + 1) % LOG_SIZE;
        --c.log_count;
    }
    sfp_port_unlock();
    return n;
}

const char *sfp_state_name(sfp_state_t s) {
    static const char *names[] = {"ABSENT", "OFF", "POWERING", "IDENTIFYING",
                                  "READY", "TX_FAULT", "INVALID", "BUS_ERROR"};
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}

const char *sfp_err_name(sfp_err_t e) {
    switch (e) {
    case SFP_OK: return "OK";
    case SFP_ERR_NACK: return "NACK";
    case SFP_ERR_BUS: return "BUS";
    case SFP_ERR_CHECKSUM: return "CHECKSUM";
    case SFP_ERR_ARG: return "ARG";
    case SFP_ERR_ABSENT: return "ABSENT";
    case SFP_ERR_NOT_READY: return "NOT_READY";
    case SFP_ERR_TIMEOUT: return "TIMEOUT";
    }
    return "?";
}

#ifdef SFP_TEST_HOOKS
#include "sfp_test_hooks.h"

void sfp_test_inject_nack(uint32_t n) { sfp_core_inject_nack(n); }

void sfp_test_inject_sda_low(uint32_t ms) { sfp_core_inject_sda_low(ms); }
#endif
