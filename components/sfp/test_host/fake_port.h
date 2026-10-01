// Host implementation of sfp_port.h: a fake SFP+ module behind a fake I2C
// bus, a fake clock, and the fault hooks the tests need. Also used by the
// simulator binary so the pytest suite can run in CI without hardware.
#ifndef FAKE_PORT_H
#define FAKE_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "sfp.h"

void fake_reset(void);
// Loads the module's A0h and A2h pages (256 bytes each).
void fake_load(const uint8_t a0[256], const uint8_t a2[256]);
uint64_t fake_now_us(void);

// Module side. These change the pins and queue the matching event, the way
// the GPIO interrupt does on the board.
void fake_insert(void);
void fake_remove(void);
void fake_set_los(bool los);
// TX_FAULT goes high and stays high until TX_DISABLE has been pulsed
// clear_after times (0 = never clears).
void fake_tx_fault(uint32_t clear_after);
// Writes live diagnostics (A2h 96-105) and the alarm bytes (112-113).
void fake_set_live(int16_t temp_c256, uint16_t vcc, uint16_t bias, uint16_t tx, uint16_t rx);
void fake_set_alarm_bytes(uint8_t b112, uint8_t b113);
// Time after power-on before the module answers on I2C.
void fake_set_module_boot_ms(uint32_t ms);

// Bus faults.
void fake_nack_next(uint32_t n);             // next n transactions NACK
void fake_stuck_sda(uint32_t clocks);        // slave holds SDA until it sees this many SCL clocks
void fake_stuck_sda_returns_zeros(bool on);  // controller reads zeros instead of timing out
void fake_corrupt_next(uint32_t n);          // flip a bit in the next n reads

// Observations.
bool fake_powered(void);
bool fake_tx_disabled(void);
uint32_t fake_transactions(void);

// Runs the owner task's loop for ms of fake time: 10 Hz poll ticks plus the
// core's timers, exactly as port/esp32 schedules them.
void fake_run_for(uint32_t ms);
// Runs until the state equals s or timeout_ms passes. Returns elapsed ms.
uint32_t fake_run_until_state(sfp_state_t s, uint32_t timeout_ms);

#endif
