// SFP+ module management SDK.
//
// One task owns the I2C bus and the module state machine; everything here is
// safe to call from any other task (the shell, tests). Pin interrupts and the
// 10 Hz poll timer never touch the bus themselves, they only queue events.
//
// Every call returns sfp_err_t. Units follow SFF-8472 so nothing is lost in
// conversion: temperature in 1/256 degC, Vcc in 100 uV, bias in 2 uA, optical
// power in 0.1 uW plus a fixed-point dBm copy in hundredths of a dB.
#ifndef SFP_H
#define SFP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFP_SDK_VERSION "1.0.0"

typedef enum {
    SFP_OK = 0,
    SFP_ERR_NACK = -1,       // module did not acknowledge after all retries
    SFP_ERR_BUS = -2,        // bus stuck and recovery failed
    SFP_ERR_CHECKSUM = -3,   // data read but CC_BASE / CC_EXT / identifier wrong
    SFP_ERR_ARG = -4,
    SFP_ERR_ABSENT = -5,     // no module in the cage
    SFP_ERR_NOT_READY = -6,  // module present but not identified yet
    SFP_ERR_TIMEOUT = -7,    // I2C controller timed out (bus held)
} sfp_err_t;

typedef enum {
    SFP_ABSENT = 0,
    SFP_OFF,           // module present, power switched off by sfp_power(false)
    SFP_POWERING,     // power on, waiting t_init (300 ms, SFF-8431)
    SFP_IDENTIFYING,   // reading A0h
    SFP_READY,         // identified, diagnostics polled at 10 Hz
    SFP_TX_FAULT,      // transmitter fault latched after 3 failed recoveries
    SFP_INVALID,       // not an SFP, or checksums never matched
    SFP_BUS_ERROR,     // bus stayed stuck after recovery and one power cycle
} sfp_state_t;

typedef struct {
    uint8_t identifier;  // 0x03 = SFP / SFP+
    char vendor[17];
    char part[17];
    char serial[17];
    uint16_t wavelength_nm;
    uint8_t bitrate_100mbps;  // 0x67 = 10.3 Gb/s
    bool ddm;                 // diagnostics implemented (A0h byte 92 bit 6)
    bool ext_cal;             // externally calibrated (bit 4)
    bool addr_change;         // A2h needs an address change sequence (bit 2)
    uint8_t compliance_10g;   // A0h byte 3: 10GBASE-SR/LR/LRM/ER bits
} sfp_identity_t;

typedef struct {
    int16_t temp_c256;
    uint16_t vcc_100uv;
    uint16_t bias_2ua;
    uint16_t tx_01uw;
    uint16_t rx_01uw;
    int16_t tx_cdbm;  // hundredths of a dBm, or SFP_NO_LIGHT
    int16_t rx_cdbm;
    bool los;         // RX_LOS pin asserted: rx power is "no light", not a number
    uint32_t age_ms;  // time since this snapshot was read
} sfp_ddm_t;

#define SFP_NO_LIGHT INT16_MIN

// One parameter's alarm and warning limits, raw SFF-8472 units.
typedef struct {
    int32_t high_alarm, low_alarm, high_warn, low_warn;
} sfp_limits_t;

typedef struct {
    sfp_limits_t temp, vcc, bias, tx_power, rx_power;
} sfp_thresholds_t;

// Alarm and warning flags, A2h bytes 112-113 (alarms) and 116-117 (warnings).
#define SFP_ALM_TEMP_HIGH  (1u << 15)
#define SFP_ALM_TEMP_LOW   (1u << 14)
#define SFP_ALM_VCC_HIGH   (1u << 13)
#define SFP_ALM_VCC_LOW    (1u << 12)
#define SFP_ALM_BIAS_HIGH  (1u << 11)
#define SFP_ALM_BIAS_LOW   (1u << 10)
#define SFP_ALM_TX_HIGH    (1u << 9)
#define SFP_ALM_TX_LOW     (1u << 8)
#define SFP_ALM_RX_HIGH    (1u << 7)
#define SFP_ALM_RX_LOW     (1u << 6)

typedef enum {
    SFP_EV_MOD_PRESENT = 1,  // MOD_ABS fell
    SFP_EV_MOD_REMOVED,      // MOD_ABS rose
    SFP_EV_LOS_ASSERT,       // RX_LOS rose
    SFP_EV_LOS_CLEAR,        // RX_LOS fell
    SFP_EV_TX_FAULT,         // TX_FAULT rose
    SFP_EV_POLL,             // 10 Hz timer
    // logged only, never queued:
    SFP_EV_STATE,            // arg = new state
    SFP_EV_FAULT_RECOVERY,   // arg = attempt number
    SFP_EV_BUS_RECOVERY,
    SFP_EV_ALARM,            // arg = alarm flags that changed
} sfp_event_type_t;

typedef struct {
    uint64_t t_us;
    sfp_event_type_t type;
    uint32_t arg;
} sfp_event_t;

typedef struct {
    uint32_t polls;
    uint32_t i2c_transfers;
    uint32_t i2c_nack;
    uint32_t i2c_timeout;
    uint32_t i2c_retries;
    uint32_t bus_recoveries;
    uint32_t checksum_fail;
    uint32_t los_events;
    uint32_t fault_events;
    uint32_t fault_recoveries;
    uint32_t power_cycles;
    uint32_t events_dropped;   // poll events dropped because the queue was full
    uint32_t last_ready_ms;    // insert (or power on) to READY, last time
    uint32_t worst_ready_ms;   // worst since stats were cleared
} sfp_stats_t;

typedef void (*sfp_event_cb_t)(const sfp_event_t *ev, void *ctx);

typedef struct {
    uint32_t poll_period_ms;   // default 100 (10 Hz)
    uint32_t debounce_ms;      // default 20
    uint32_t t_init_ms;        // default 300 (SFF-8431 t_init)
} sfp_config_t;

// 1. Start the SDK. NULL config uses the defaults above.
sfp_err_t sfp_init(const sfp_config_t *cfg);
// 2.
sfp_err_t sfp_get_state(sfp_state_t *state);
// 3.
sfp_err_t sfp_get_identity(sfp_identity_t *id);
// 4. Latest diagnostics snapshot (never a bus read, so it never blocks).
sfp_err_t sfp_get_ddm(sfp_ddm_t *ddm);
// 5.
sfp_err_t sfp_get_thresholds(sfp_thresholds_t *th);
// 6. Bits SFP_ALM_* currently set; warnings in the high half-word shifted by 16.
sfp_err_t sfp_get_alarms(uint32_t *flags);
// 7. Hardware TX_DISABLE pin.
sfp_err_t sfp_set_tx_disable(bool disable);
// 8. Module power through the load switch. Off clears identity and state.
sfp_err_t sfp_power(bool on);
// 9. Raw read of page 0x50 (A0h) or 0x51 (A2h), done by the owner task.
sfp_err_t sfp_read_raw(uint8_t dev, uint8_t offset, uint8_t *buf, size_t n);
// 10. One callback, called from the owner task for every logged event.
sfp_err_t sfp_register_callback(sfp_event_cb_t cb, void *ctx);
// 11.
sfp_err_t sfp_get_stats(sfp_stats_t *stats);
sfp_err_t sfp_clear_stats(void);
// 12. Copies up to max logged events, oldest first, and removes them.
size_t sfp_events_read(sfp_event_t *out, size_t max);

const char *sfp_state_name(sfp_state_t s);
const char *sfp_err_name(sfp_err_t e);

#ifdef __cplusplus
}
#endif

#endif
