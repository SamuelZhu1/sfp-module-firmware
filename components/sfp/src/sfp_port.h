// Platform layer under the SDK. The core (transport, decoder, state machine)
// only talks to the hardware through these calls, so the same core runs on
// the ESP32-C3 (port/esp32) and on a Linux host against a fake module
// (test_host/fake_port.c) for unit tests and the simulator.
#ifndef SFP_PORT_H
#define SFP_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sfp.h"

typedef enum {
    SFP_PORT_OK = 0,
    SFP_PORT_NACK,     // address or data byte not acknowledged
    SFP_PORT_TIMEOUT,  // controller could not finish (SDA or SCL held)
} sfp_port_xfer_t;

// One combined transaction: START, dev+W, offset, repeated START, dev+R,
// n bytes, STOP. Multi-byte values come from one transfer, never two.
sfp_port_xfer_t sfp_port_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n);

// Line levels as seen on the pins (1 = high / released).
int sfp_port_sda(void);
int sfp_port_scl(void);

// Bus recovery: take SDA and SCL away from the I2C controller as open-drain
// GPIOs, drive them by hand, then give them back and reinstall the driver.
void sfp_port_bb_begin(void);
void sfp_port_bb_drive(int scl_high, int sda_high);
void sfp_port_bb_end(void);

void sfp_port_delay_us(uint32_t us);
uint64_t sfp_port_now_us(void);

// Module status and control pins.
bool sfp_port_mod_present(void);  // MOD_ABS low
bool sfp_port_rx_los(void);
bool sfp_port_tx_fault(void);
void sfp_port_set_tx_disable(bool disable);
void sfp_port_set_power(bool on);

// Guards the bus and the module snapshot. Called by the core only.
void sfp_port_lock(void);
void sfp_port_unlock(void);

// Configures pins, the I2C bus, the lock and the event queue. Runs before
// the core is initialised (the core reads MOD_ABS during init).
void sfp_port_init(void);
// Starts pin interrupts, the poll timer and the owner task.
// The host port does nothing in either call; tests drive the core directly.
void sfp_port_start(const sfp_config_t *cfg);

// Test builds only: hold SDA low through a spare GPIO for ms milliseconds,
// the way a module pulled mid-byte does.
void sfp_port_hold_sda_low(uint32_t ms);

#endif
