# Design spec

## Goal

Manage one SFP+ cage from a microcontroller with the behaviour a switch's
module manager would have: correct power-up timing, verified identity data,
fresh diagnostics, and recovery from the faults that actually happen on the
low-speed bus (a module that NACKs while busy, a slave stuck holding SDA, a
corrupt read, a laser that trips).

## Requirements

| ID | Requirement | Where it's tested |
| --- | --- | --- |
| R1 | Ignore MOD_ABS bounces shorter than 20 ms | test_state: bounce |
| R2 | No I2C access until 300 ms after power (SFF-8431 t_init) | test_state: insert; HIL insert events |
| R3 | Reject identity data whose CC_BASE or CC_EXT checksum fails, after 3 reads | test_state: checksum retry, INVALID |
| R4 | Reject an all-zero EEPROM read (it passes both checksums) | test_decode; fault campaign |
| R5 | Diagnostics at 10 Hz, never older than 150 ms while READY | test_state: polls; HIL poll rate |
| R6 | Diagnostics available the moment READY is reported | test_state: insert; HIL ddm |
| R7 | Retry a failed transfer up to 4 times with 2/4/8 ms backoff | test_i2c |
| R8 | Free a bus held low by a slave with up to 9 SCL clocks and a STOP | test_i2c; fault campaign; HIL |
| R9 | 3 failed polls: one power cycle, then BUS_ERROR with power off | test_state |
| R10 | TX_FAULT: up to 3 reset pulses, then latch with laser off | test_state; HIL |
| R11 | RX_LOS clears only after 50 ms stable | test_state; HIL |
| R12 | Pulling the module powers the cage off and clears data immediately | test_state; HIL |
| R13 | Optical power in dBm within 0.01 dB of floating point, no bias | test_decode (all 65,535 inputs); HIL ethtool cross-check |
| R14 | Survive repeated power cycles and injected bus faults without a power cycle of its own | HIL power-cycle soak, fault campaign |

## Threads and ownership

On the chip there is exactly one task that touches the bus and the state
machine (the owner task). Everything else talks to it:

- **GPIO ISR** (MOD_ABS, RX_LOS, TX_FAULT edges): takes an `esp_timer`
  timestamp, toggles the debug pin on RX_LOS so the latency can be scoped, and
  posts an event to a queue. It never touches I2C.
- **Poll timer** (`esp_timer`, 100 ms): posts a poll tick. If fewer than 4
  queue slots are free it drops the tick and counts it, so a backed-up queue
  can't lose a plug or fault edge to a flood of polls.
- **Owner task**: waits on the queue with a timeout equal to the next pending
  deadline (debounce end, t_init end, fault re-check, LOS clear), so timers
  don't need their own callbacks into the state machine.
- **SDK callers** (the shell, or any other task): read cached data under a
  recursive mutex. They never wait on the bus; `sfp_read_raw` is the only call
  that does a live read, and it takes the same lock.

The host build replaces the queue and timers with a fake clock and a loop
(`fake_run_for`) that delivers the same events in the same order, which is
what makes timing tests exact.

## I2C transport

- 100 kHz, 7-bit addresses 0x50 (A0h) and 0x51 (A2h), 20 ms transfer timeout.
- Before each transfer, if SDA reads low the bus is recovered first: some
  controllers silently return all zeros when they can't send START.
- Recovery takes the pins from the I2C driver, drives them open drain, clocks
  SCL until SDA is released (at most 9 clocks), sends a STOP and hands the pins
  back. Still low after 9 clocks is a bus error.
- Retries back off 2, 4, 8 ms so a module still busy writing (EEPROM write
  cycle is up to 5 ms) gets time to come back.

## Data conversion

- Temperature: signed 1/256 degC. Supply: 100 uV. Bias: 2 uA. Optical power:
  0.1 uW. All stored as integers.
- dBm = 10 log10(P / 1 mW). Computed as `10 * log10(2) * log2(P)` with a
  65-entry (64-segment) log2 table and linear interpolation in Q16 fixed
  point. Against `log10` rounded to hundredths, over all 65,535 inputs: never
  more than 0.01 dB apart, and only 274 values off by that 0.01 (266 low, 8
  high). Zero power is reported as
  "no light" (JSON null), not as a huge negative number.
- External calibration (byte 92 bit 4): slope/offset for bias, TX power,
  temperature and supply; a 4th-order float polynomial for RX power, as
  SFF-8472 specifies. This is the only floating point in the SDK.

## Failure handling summary

| Fault | Detection | Action |
| --- | --- | --- |
| NACK (busy module) | transfer result | retry with backoff |
| SDA held by slave | SDA low before START, or after a timeout | 9-clock recovery, retry |
| Corrupt identity read | checksum mismatch | re-read up to 3 times, then INVALID |
| All-zero read | identifier byte 0x00 | INVALID (checksums alone pass it) |
| Repeated poll failures | 3 in a row | one power cycle, then BUS_ERROR |
| Laser fault | TX_FAULT high | up to 3 TX_DISABLE pulses, then latch, laser off |
| Module pulled | MOD_ABS high | power off, clear data, ABSENT |

## Not in scope

- Paged memory (SFF-8472 page select at A2h byte 127) and QSFP modules.
- Writing the module's EEPROM, other than TX_DISABLE through the pin.
- More than one cage.
