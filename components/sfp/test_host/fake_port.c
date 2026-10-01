#include "fake_port.h"

#include <string.h>

#include "../src/sfp_internal.h"
#include "../src/sfp_port.h"

#define POLL_US 100000ull
#define I2C_BIT_US 10  // 100 kHz

static struct {
    uint64_t t;
    uint64_t next_poll;
    bool present, powered, tx_disable, los, tx_fault;
    uint64_t powered_at;
    uint32_t boot_us;
    uint8_t a0[256], a2[256];
    uint32_t nack_next, corrupt_next;
    uint32_t stuck_clocks;   // slave releases SDA after this many SCL rising edges
    bool stuck_zeros;
    uint64_t hold_until;     // spare GPIO holding SDA low
    bool bb, bb_scl;
    uint32_t fault_clear_after, fault_pulses;
    uint32_t transactions;
} f;

void fake_reset(void) {
    memset(&f, 0, sizeof f);
    f.t = 1000000;  // start at 1 s so "time 0" never looks like "never"
    f.next_poll = f.t + POLL_US;
}

void fake_load(const uint8_t a0[256], const uint8_t a2[256]) {
    memcpy(f.a0, a0, 256);
    memcpy(f.a2, a2, 256);
}

uint64_t fake_now_us(void) { return f.t; }

static void post(sfp_event_type_t type) {
    sfp_event_t ev = {.t_us = f.t, .type = type};
    sfp_core_handle(&ev);
}

void fake_insert(void) {
    f.present = true;
    post(SFP_EV_MOD_PRESENT);
}

void fake_remove(void) {
    f.present = false;
    post(SFP_EV_MOD_REMOVED);
}

void fake_set_los(bool los) {
    if (los == f.los) return;
    f.los = los;
    post(los ? SFP_EV_LOS_ASSERT : SFP_EV_LOS_CLEAR);
}

void fake_tx_fault(uint32_t clear_after) {
    f.fault_clear_after = clear_after;
    f.fault_pulses = 0;
    f.tx_fault = true;
    post(SFP_EV_TX_FAULT);
}

static void be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void fake_set_live(int16_t temp, uint16_t vcc, uint16_t bias, uint16_t tx, uint16_t rx) {
    be16(f.a2 + 96, (uint16_t)temp);
    be16(f.a2 + 98, vcc);
    be16(f.a2 + 100, bias);
    be16(f.a2 + 102, tx);
    be16(f.a2 + 104, rx);
}

void fake_set_alarm_bytes(uint8_t b112, uint8_t b113) {
    f.a2[112] = b112;
    f.a2[113] = b113;
}

void fake_set_module_boot_ms(uint32_t ms) { f.boot_us = ms * 1000; }
void fake_nack_next(uint32_t n) { f.nack_next = n; }
void fake_stuck_sda(uint32_t clocks) { f.stuck_clocks = clocks; }
void fake_stuck_sda_returns_zeros(bool on) { f.stuck_zeros = on; }
void fake_corrupt_next(uint32_t n) { f.corrupt_next = n; }
bool fake_powered(void) { return f.powered; }
bool fake_tx_disabled(void) { return f.tx_disable; }
uint32_t fake_transactions(void) { return f.transactions; }

// ---------------- sfp_port.h ----------------

static bool sda_held(void) { return f.stuck_clocks > 0 || f.t < f.hold_until; }

int sfp_port_sda(void) { return sda_held() ? 0 : 1; }

int sfp_port_scl(void) { return f.bb ? f.bb_scl : 1; }

sfp_port_xfer_t sfp_port_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    ++f.transactions;
    f.t += (uint64_t)(n + 4) * 9 * I2C_BIT_US;  // bytes on the wire at 100 kHz
    if (sda_held()) {
        if (f.stuck_zeros) {
            memset(buf, 0, n);  // what the RISC-V core's controller returned
            return SFP_PORT_OK;
        }
        return SFP_PORT_TIMEOUT;
    }
    if (!f.present || !f.powered || f.t < f.powered_at + f.boot_us) return SFP_PORT_NACK;
    if (f.nack_next) {
        --f.nack_next;
        return SFP_PORT_NACK;
    }
    const uint8_t *page = dev == SFP_A0 ? f.a0 : dev == SFP_A2 ? f.a2 : NULL;
    if (!page) return SFP_PORT_NACK;
    memcpy(buf, page + offset, n);
    if (f.corrupt_next) {
        --f.corrupt_next;
        buf[n / 2] ^= 0x10;
    }
    return SFP_PORT_OK;
}

void sfp_port_bb_begin(void) {
    f.bb = true;
    f.bb_scl = true;
}

void sfp_port_bb_drive(int scl_high, int sda_high) {
    (void)sda_high;
    if (scl_high && !f.bb_scl && f.stuck_clocks) --f.stuck_clocks;  // rising edge
    f.bb_scl = scl_high;
}

void sfp_port_bb_end(void) { f.bb = false; }

void sfp_port_delay_us(uint32_t us) { f.t += us; }

uint64_t sfp_port_now_us(void) { return f.t; }

bool sfp_port_mod_present(void) { return f.present; }
bool sfp_port_rx_los(void) { return f.los; }
bool sfp_port_tx_fault(void) { return f.tx_fault; }

void sfp_port_set_tx_disable(bool disable) {
    // a full assert/release pulse is what resets the transmitter
    if (f.tx_disable && !disable && f.tx_fault && f.fault_clear_after) {
        if (++f.fault_pulses >= f.fault_clear_after) f.tx_fault = false;
    }
    f.tx_disable = disable;
}

void sfp_port_set_power(bool on) {
    if (on && !f.powered) f.powered_at = f.t;
    f.powered = on;
}

void sfp_port_lock(void) {}
void sfp_port_unlock(void) {}
void sfp_port_init(void) {}
void sfp_port_start(const sfp_config_t *cfg) { (void)cfg; }

void sfp_port_hold_sda_low(uint32_t ms) { f.hold_until = f.t + (uint64_t)ms * 1000; }

// ---------------- scheduler ----------------

void fake_run_for(uint32_t ms) {
    uint64_t end = f.t + (uint64_t)ms * 1000;
    for (;;) {
        uint64_t next = f.next_poll;
        uint64_t d = sfp_core_next_deadline_us();
        if (d < next) next = d;
        if (next > end) break;
        if (next > f.t) f.t = next;
        if (f.t >= f.next_poll) {
            f.next_poll += POLL_US;
            sfp_event_t ev = {.t_us = f.t, .type = SFP_EV_POLL};
            sfp_core_handle(&ev);
        }
        sfp_core_service();
    }
    if (end > f.t) f.t = end;
}

uint32_t fake_run_until_state(sfp_state_t s, uint32_t timeout_ms) {
    uint64_t start = f.t;
    for (uint32_t i = 0; i < timeout_ms; ++i) {
        sfp_state_t now;
        sfp_get_state(&now);
        if (now == s) break;
        fake_run_for(1);
    }
    return (uint32_t)((f.t - start) / 1000);
}
