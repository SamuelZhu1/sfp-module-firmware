// Interfaces between the SDK's own source files. Not part of the public API.
#ifndef SFP_INTERNAL_H
#define SFP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sfp.h"

#define SFP_A0 0x50
#define SFP_A2 0x51

// ---- transport (sfp_i2c.c) ----
typedef struct {
    uint32_t transfers, nacks, timeouts, retries, bus_recoveries;
} sfp_i2c_stats_t;

// Read with up to 3 retries (2, 4, 8 ms backoff) and bus recovery when SDA
// is found held low.
sfp_err_t sfp_i2c_read(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n);
sfp_err_t sfp_i2c_bus_recover(void);
const sfp_i2c_stats_t *sfp_i2c_stats(void);
void sfp_i2c_clear_stats(void);
// Test hook: the next n transfer attempts fail as if NACKed.
void sfp_i2c_inject_nack(uint32_t n);

// ---- SFF-8472 decoding (sfp_decode.c), pure functions ----
uint8_t sfp_sum8(const uint8_t *p, size_t n);
// a0 is A0h bytes 0-95. Checks identifier 0x03, CC_BASE and CC_EXT.
sfp_err_t sfp_decode_identity(const uint8_t a0[96], sfp_identity_t *id);
int16_t sfp_power_to_cdbm(uint16_t power_01uw);
// a2 is A2h bytes 0-39 (thresholds).
void sfp_decode_thresholds(const uint8_t a2[40], sfp_thresholds_t *th);

// External calibration constants, A2h bytes 56-91.
typedef struct {
    float rx_pwr[5];             // Rx_PWR(4)..Rx_PWR(0) as stored, IEEE-754
    uint16_t bias_slope, tx_slope, temp_slope, vcc_slope;  // unsigned 8.8
    int16_t bias_off, tx_off, temp_off, vcc_off;
} sfp_ext_cal_t;
void sfp_decode_ext_cal(const uint8_t a2_56_91[36], sfp_ext_cal_t *cal);

// live is A2h bytes 96-117 (diagnostics, status, alarm and warning flags).
// cal may be NULL for internally calibrated modules.
void sfp_decode_ddm(const uint8_t live[22], const sfp_ext_cal_t *cal, sfp_ddm_t *d,
                    uint32_t *alarm_flags, uint8_t *status);

// ---- state machine (sfp_core.c) ----
void sfp_core_init(const sfp_config_t *cfg);
// Handles one queued event (pin edge or poll tick). Takes the lock.
void sfp_core_handle(const sfp_event_t *ev);
// Runs timers that are due (debounce, t_init, fault recovery, LOS clear).
void sfp_core_service(void);
// Absolute time of the next timer, or UINT64_MAX if none is armed.
uint64_t sfp_core_next_deadline_us(void);
// Fault injection for the test build.
void sfp_core_inject_nack(uint32_t n);
void sfp_core_inject_sda_low(uint32_t ms);
// The owner task's queue was full and a poll tick was dropped.
void sfp_core_note_dropped(void);

#endif
