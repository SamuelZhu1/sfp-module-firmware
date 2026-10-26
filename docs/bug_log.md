# Bug log

Bugs found while building this, how they showed up, and the fix. All of these
were found in host tests or the simulator; bench findings will be added as
they come.

## 1. Alarm names off by one slot

**Symptom.** `test_shell` forced a temperature high alarm and the shell
reported `tx_high`.

**Cause.** The name table in `sfp_shell.c` was laid out in the order of the
alarm bytes as printed in SFF-8472 (temperature first), but indexed by bit
number counting up from bit 6 (RX power low first).

**Fix.** Ten entries in bit order, indexed by `bit - 6`. The test now forces
each alarm in turn.

## 2. MOD_ABS read before its pin was configured

**Symptom.** Found while writing the ESP32 port: `sfp_core_init` reads MOD_ABS
to learn whether a module is already in the cage at boot, but the GPIOs were
only configured when the owner task started, after that read. An unconfigured
pin reads whatever it floats to.

**Fix.** Split the port into `sfp_port_init` (pins, I2C bus, lock) before the
core starts and `sfp_port_start` (ISR, timer, task) after.

## 3. READY reported with no diagnostics yet

**Symptom.** The pytest suite asked for `ddm` right after seeing READY and got
`{"error":"NOT_READY"}`. A host doing the same would see a ready module with
no data for up to 100 ms.

**Cause.** The first diagnostics read waited for the next 100 ms poll tick.

**Fix.** `identify()` does the first read itself, under the same lock, before
anyone can observe READY. The unit test now checks `sfp_get_ddm` succeeds the
moment READY appears.

## 4. Poll counter counted empty ticks

**Symptom.** In a simulator session with no module, `stats` showed 4 polls
after 400 ms.

**Fix.** Count a poll only when a diagnostics read actually happens.

## 5. SDA hold test never touched the bus (test bug)

**Symptom.** The HIL test held SDA low for 5 ms and expected a bus recovery;
it saw none.

**Cause.** Polls are 100 ms apart, so the 5 ms hold ended before the next
transfer. The firmware was fine.

**Fix.** Hold for 120 ms so at least one poll meets it, and also check that it
was cleared by bus recovery alone (no power cycle).

## Design note: an all-zero EEPROM passes both checksums

The sum of 63 zero bytes is zero, which matches a zero checksum byte. A
controller that can't send START because SDA is stuck can hand back all zeros
with no error, so checksums alone would accept a blank module. The identifier
byte (must be 0x03) catches it, and `test_fault_campaign` checks that a stuck
bus returning zeros never reaches READY.
