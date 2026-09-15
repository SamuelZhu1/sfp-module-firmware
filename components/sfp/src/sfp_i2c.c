// I2C transport for SFP+ modules: one read transaction with retries, backoff
// and bus recovery. Started as the driver for the RISC-V core's memory-mapped
// controller (riscv-lockstep-core, sw/i2c_driver); here the byte-level
// controller is replaced by a transaction call into the platform layer.
#include "sfp_internal.h"
#include "sfp_port.h"

#define MAX_ATTEMPTS 4          // first try plus 3 retries
#define BACKOFF_UNIT_US 1000    // retries wait 2, 4, 8 ms
#define BB_HALF_PERIOD_US 5     // 100 kHz while bit-banging

static sfp_i2c_stats_t stats;
static uint32_t inject_nacks;

sfp_err_t sfp_i2c_bus_recover(void) {
    ++stats.bus_recoveries;
    // A slave reset or pulled mid-byte keeps driving SDA low while it waits
    // for the rest of its byte. Up to 9 clocks let it finish and release SDA.
    sfp_port_bb_begin();
    sfp_port_bb_drive(0, 1);
    sfp_port_delay_us(BB_HALF_PERIOD_US);
    for (int i = 0; i < 9 && !sfp_port_sda(); ++i) {
        sfp_port_bb_drive(1, 1);
        sfp_port_delay_us(BB_HALF_PERIOD_US);
        sfp_port_bb_drive(0, 1);
        sfp_port_delay_us(BB_HALF_PERIOD_US);
    }
    // STOP: SDA low while SCL is low, release SCL, then release SDA.
    sfp_port_bb_drive(0, 0);
    sfp_port_delay_us(BB_HALF_PERIOD_US);
    sfp_port_bb_drive(1, 0);
    sfp_port_delay_us(BB_HALF_PERIOD_US);
    sfp_port_bb_drive(1, 1);
    sfp_port_delay_us(BB_HALF_PERIOD_US);
    int ok = sfp_port_sda() && sfp_port_scl();
    sfp_port_bb_end();
    return ok ? SFP_OK : SFP_ERR_BUS;
}

static sfp_err_t read_once(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    // With SDA held low the controller can't even send START, and some
    // controllers then hand back all zeros instead of an error. Check first.
    if (!sfp_port_sda()) {
        if (sfp_i2c_bus_recover() != SFP_OK) return SFP_ERR_BUS;
    }
    if (inject_nacks) {
        --inject_nacks;
        ++stats.nacks;
        return SFP_ERR_NACK;
    }
    switch (sfp_port_i2c_read(dev, offset, buf, n)) {
    case SFP_PORT_OK:
        return SFP_OK;
    case SFP_PORT_NACK:
        ++stats.nacks;
        return SFP_ERR_NACK;
    case SFP_PORT_TIMEOUT:
    default:
        ++stats.timeouts;
        // the bus may have been grabbed mid-transfer; free it now so the
        // retry starts clean
        if (!sfp_port_sda() && sfp_i2c_bus_recover() != SFP_OK) return SFP_ERR_BUS;
        return SFP_ERR_TIMEOUT;
    }
}

sfp_err_t sfp_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n) {
    if (!buf || n == 0 || (size_t)offset + n > 256) return SFP_ERR_ARG;
    sfp_err_t err = SFP_ERR_NACK;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        if (attempt) {
            ++stats.retries;
            sfp_port_delay_us((uint32_t)BACKOFF_UNIT_US << attempt);  // 2, 4, 8 ms
        }
        ++stats.transfers;
        err = read_once(dev, offset, buf, n);
        if (err == SFP_OK) return SFP_OK;
    }
    return err;
}

const sfp_i2c_stats_t *sfp_i2c_stats(void) { return &stats; }

void sfp_i2c_clear_stats(void) { stats = (sfp_i2c_stats_t){0}; }

void sfp_i2c_inject_nack(uint32_t n) { inject_nacks = n; }
