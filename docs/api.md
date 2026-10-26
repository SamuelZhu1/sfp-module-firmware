# SDK API

Header: `components/sfp/include/sfp.h`. Every call returns `sfp_err_t`
(`SFP_OK` is 0) unless noted, and is safe to call from any task.

## Setup

| Call | What it does |
| --- | --- |
| `sfp_init(const sfp_config_t *cfg)` | Configures pins, starts the owner task and poll timer. `NULL` uses defaults: 100 ms poll, 20 ms debounce, 300 ms t_init. |
| `sfp_register_callback(cb, ctx)` | Called from the owner task for every event. Keep it short; don't call back into the bus from it. |

## Reading

| Call | Returns | Errors |
| --- | --- | --- |
| `sfp_get_state(&s)` | ABSENT, OFF, POWERING, IDENTIFYING, READY, TX_FAULT, INVALID, BUS_ERROR | |
| `sfp_get_identity(&id)` | vendor, part, serial, wavelength, bit rate, DDM and calibration flags | ABSENT, NOT_READY |
| `sfp_get_ddm(&d)` | latest diagnostics, LOS flag, age in ms | ABSENT, NOT_READY |
| `sfp_get_thresholds(&th)` | alarm and warning limits for all five values | ABSENT, NOT_READY |
| `sfp_get_alarms(&flags)` | `SFP_ALM_*` bits; warnings are the same bits shifted left 16 | ABSENT, NOT_READY |
| `sfp_read_raw(dev, off, buf, n)` | live read of A0h (0x50) or A2h (0x51) | ARG, ABSENT, NACK, TIMEOUT, BUS |
| `sfp_get_stats(&s)` / `sfp_clear_stats()` | counters: polls, transfers, NACKs, timeouts, retries, bus recoveries, checksum failures, LOS and fault events, power cycles, dropped events, last and worst insert-to-ready time | |
| `sfp_events_read(buf, max)` | drains up to `max` events from the 64-entry log; returns the count (`size_t`) | |

## Control

| Call | What it does |
| --- | --- |
| `sfp_set_tx_disable(bool)` | drives TX_DISABLE |
| `sfp_power(bool)` | powers the cage off (state OFF) or back on (full power-up and identify) |

## Events

`MOD_PRESENT`, `MOD_REMOVED`, `LOS`, `LOS_CLEAR`, `TX_FAULT`, `STATE`
(arg = new state), `FAULT_RECOVERY` (arg = attempt), `BUS_RECOVERY`, `ALARM`
(arg = flags that changed). Each carries a microsecond timestamp.

## Porting

A new platform implements `src/sfp_port.h`: an I2C read that reports OK, NACK
or timeout; raw SDA/SCL access for bus recovery; a microsecond clock and delay;
the five module pins; a recursive lock; and `sfp_port_init` / `sfp_port_start`.
`port/esp32/sfp_port_esp32.c` is the reference, and `test_host/fake_port.c` is
the version the tests use.
