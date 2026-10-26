# SFP+ Optical Module Management Firmware

Firmware for an ESP32-C3 (RISC-V) that manages a 10G SFP+ optical transceiver
the way a switch or NIC does: it detects the module being plugged in, powers it
up with the timing SFF-8431 requires, reads and checks its identity EEPROM,
polls the live diagnostics (temperature, supply voltage, laser bias, transmit
and receive optical power) at 10 Hz, raises alarms against the module's own
thresholds, and recovers from I2C bus faults and laser faults on its own.

It is written as a small C SDK (`components/sfp`) with a platform port layer,
so the same core runs on the chip, in the host unit tests, and in a simulator
that CI drives with the same pytest suite used on the bench.

```
module plugged in
      |
  MOD_ABS edge (GPIO ISR) --> queue --> owner task --> state machine
                                                  |
   ABSENT -> POWERING (t_init 300 ms) -> IDENTIFYING -> READY <-> TX_FAULT
                                              |            \
                                           INVALID       BUS_ERROR
```

## What it does

- **Hot-plug handling.** MOD_ABS is debounced for 20 ms, then the module is
  powered through a load switch and left alone for the SFF-8431 300 ms t_init
  before the first read. Pulling the module at any point powers the cage off
  and clears all data immediately.
- **Identity (SFF-8472 page A0h).** Vendor, part number, serial, wavelength and
  bit rate, with both checksums (CC_BASE, CC_EXT) verified and the identifier
  byte checked. A corrupt read is retried up to 3 times before the module is
  marked INVALID.
- **Diagnostics (page A2h).** One 22-byte burst read every 100 ms covers all
  five live values plus the status and alarm bytes. Optical power is converted
  from microwatts to dBm in fixed point (table-based log2, worst error 0.02 dB
  across the range). Internally and externally calibrated modules are both
  supported.
- **Alarms.** The module's own alarm and warning thresholds are read at
  identification; changes in the alarm flags are logged as events.
- **I2C fault recovery.** Each transfer is retried up to 4 times with 2, 4 and
  8 ms backoff. If a slave is holding SDA low, the transport clocks SCL up to
  9 times and sends a STOP to free the bus before trying again. Three failed
  polls in a row trigger one power cycle; if that doesn't fix it, the state
  goes to BUS_ERROR with the module powered off.
- **Laser faults.** TX_FAULT gets up to 3 SFF-8431 reset pulses on TX_DISABLE;
  a fault that survives them is latched with the laser off.
- **Receive loss of signal.** RX_LOS edges are timestamped in the ISR and the
  flag clears only after 50 ms of stable light.

## Layout

```
components/sfp/
  include/sfp.h            public SDK
  src/sfp_core.c           state machine, timers, event log
  src/sfp_i2c.c            transfers, retries, bus recovery
  src/sfp_decode.c         SFF-8472 parsing and unit conversion
  src/sfp_port.h           what a platform has to provide
  port/esp32/              ESP-IDF port: i2c_master, GPIO ISR, esp_timer, FreeRTOS
  test_host/               Unity unit tests, fake module, simulator
components/shell/          JSON line shell over USB Serial/JTAG
main/app_main.c            starts the SDK and the shell
tests_hil/                 pytest suite: board over USB, or the simulator
docs/                      design spec, API, bring-up guide, bug log
```

## Build and test

Host tests and the simulator need only GCC and Python:

```
make -C components/sfp/test_host test       # 49 unit tests
make -C components/sfp/test_host coverage   # line coverage (pip install gcovr)
make -C components/sfp/test_host sim
python3 -m pytest tests_hil                 # 20 tests against the simulator
```

Firmware (ESP-IDF v5.2.2):

```
idf.py set-target esp32c3 build
idf.py -p /dev/ttyACM0 flash monitor
```

The test build adds fault injection commands to the shell; `scripts/run_bench.sh`
flashes it and runs the same pytest suite on the board:

```
scripts/run_bench.sh /dev/ttyACM0 --interactive
```

## Shell

Every command returns one JSON line, so scripts and the test suite can parse it.

```
> state
{"state":"READY"}
> id
{"vendor":"FINISAR CORP.","part":"FTLX1471D3BCL","serial":"UZ80TQ2","wavelength_nm":1310,"bitrate_mbps":10300,"ddm":true,"ext_cal":false}
> ddm
{"temp_c":35.50,"vcc_v":3.3000,"bias_ma":6.800,"tx_dbm":-2.41,"rx_dbm":-5.11,"los":false,"age_ms":3}
```

Commands: `state`, `id`, `ddm`, `thresholds`, `alarms`, `txdis on|off`,
`power on|off`, `raw a0|a2 <offset> <len>`, `events`, `stats [clear]`,
`version`, and in the test build `inject nack <n>` and `inject sda_low <ms>`.

## Status

| Part | Status |
| --- | --- |
| SDK core, decoder, transport | done; 49 unit tests, 86% line coverage |
| Simulator and pytest suite | done; 20 tests pass, 1,000-cycle hot-plug soak in simulation with 0 failures |
| ESP32-C3 port | compiles under ESP-IDF 5.2.2 with -Werror; not yet run on hardware |
| Bench bring-up with real modules | next; see `docs/bringup.md` |

Numbers above come from the host simulator, not from hardware. Bench results
will be added to `docs/bringup.md` as they are measured.

## Standards

- SFF-8472 Rev 12.4: Management Interface for SFP+ (EEPROM map, diagnostics)
- SFF-8431 Rev 4.1: SFP+ electrical (timing, TX_DISABLE, TX_FAULT, RX_LOS)
- SFF-8419: SFP+ power and low speed interface
