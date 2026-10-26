# Bench bring-up

## Hardware

- ESP32-C3-DevKitM-1
- SFP+ cage with a 20-pin connector breakout
- Load switch for module power (e.g. TPS22918), 3.3 V rail good for 600 mA
  (a 10G LR module draws up to 1.5 W)
- 4.7 kOhm pull-ups to 3.3 V on SDA, SCL, MOD_ABS, RX_LOS and TX_FAULT
  (SFF-8431 host side: 4.7 to 10 kOhm)
- 220 Ohm resistor from the spare GPIO to SDA (fault injection)
- Modules: one internally calibrated 10GBASE-LR, one 10GBASE-SR, ideally one
  externally calibrated; an LC loopback patch or a short duplex fibre
- Logic analyser on SDA, SCL and the debug pin

## Wiring (Kconfig defaults)

| Signal | GPIO | Notes |
| --- | --- | --- |
| SDA | 6 | |
| SCL | 7 | |
| MOD_ABS | 3 | low = present. Not GPIO2: a module left in the cage would hold the strapping pin low at reset |
| RX_LOS | 4 | |
| TX_FAULT | 5 | |
| TX_DISABLE | 20 | |
| POWER | 10 | load switch enable |
| DEBUG | 21 | toggled in the RX_LOS ISR |
| SDA_HOLD | 2 | open drain, released at boot so the strap still reads high |

Avoid GPIO 8 and 9 too (strapping pins on the C3).

## Steps

1. Flash the test build: `scripts/run_bench.sh /dev/ttyACM0` flashes and runs
   the suite; for a first look use `idf.py -p /dev/ttyACM0 flash monitor`.
2. With no module: `state` should say ABSENT and the load switch output should
   be off.
3. Insert a module. On the analyser, confirm no SCL activity for 300 ms after
   the power enable rises, then a 96-byte read at 0x50 and one at 0x51.
4. `id` and `raw a0 0 128`: compare against `ethtool -m <if> raw on hex on`
   from the same module in a NIC, or the vendor datasheet.
5. `ddm` with the loopback patch in place: TX and RX power within a dB or two
   of each other, supply near 3.3 V.
6. Pull the fibre and watch `events` for LOS; scope the debug pin against
   RX_LOS for ISR latency.
7. `inject nack 3` and `inject sda_low 120`: `stats` should show the retries
   and bus recoveries with the state staying READY. Capture the 9-clock
   recovery on the analyser for the record.
8. Run the full suite: `scripts/run_bench.sh /dev/ttyACM0 --interactive`, and
   a soak with `--cycles 100` if a relay fixture is wired to the module's
   power and MOD_ABS lines.
9. Dump each module (`raw a0 0 128`, `raw a2 0 128`) into
   `test_host/fixtures/` so the host tests run against real images too.

## Results

To be filled in from the bench. Each row gets the date, the module and how it
was measured.

| Measurement | Simulator | Bench |
| --- | --- | --- |
| Insert to READY | 338 ms | |
| Diagnostics poll rate | 10 Hz | |
| RX_LOS edge to ISR (debug pin) | n/a | |
| NACK burst recovery | 200 of 200 | |
| Stuck SDA recovery | 200 of 200 | |
| Hot-plug soak | 1,000 cycles, 0 failures | |
