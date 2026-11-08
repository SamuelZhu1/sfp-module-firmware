"""Long runs: power-cycle soak (unattended on the board) and hot-plug soak."""
import statistics

import pytest


@pytest.mark.slow
def test_power_cycle_soak(ready, request):
    """Cycle cage power through the load switch and re-identify every time.

    Needs no one at the bench: `power off` / `power on` drive the load switch.
    On the board: scripts/run_bench.sh /dev/ttyACM0 -k power_cycle --power-cycles 1000
    """
    cycles = request.config.getoption("--power-cycles")
    times, failures = [], []
    for i in range(cycles):
        ready.ok("power off")
        if ready.cmd("state")["state"] != "OFF":
            failures.append((i, "did not go OFF"))
        ready.ok("power on")
        try:
            ready.wait_for_state("READY", 1000)
            d = ready.cmd("ddm")
            assert "vcc_v" in d, d
            times.append(ready.cmd("stats")["last_ready_ms"])
        except AssertionError as e:
            failures.append((i, str(e)))
            ready.ok("power off")  # get back to a known state and keep going
            ready.ok("power on")
            ready.wait(1000)
    if times:
        print(f"\n{cycles} power cycles, {len(failures)} failures, power-on to READY "
              f"median {statistics.median(times)} ms, worst {max(times)} ms")
    assert not failures, failures[:5]
    assert max(times) < 400


@pytest.mark.slow
def test_hot_plug_soak(dut, request):
    """Plug, identify, read, pull. On the board this needs the relay fixture
    or a person (--interactive), so CI runs it in the simulator."""
    cycles = request.config.getoption("--cycles")
    worst = 0
    failures = []
    for i in range(cycles):
        dut.insert()
        try:
            dut.wait_for_state("READY", 1000)
            assert "vcc_v" in dut.cmd("ddm")
        except AssertionError as e:
            failures.append((i, str(e)))
        worst = max(worst, dut.cmd("stats")["last_ready_ms"])
        dut.remove()
        dut.wait_for_state("ABSENT", 100)
    print(f"\n{cycles} hot-plug cycles, {len(failures)} failures, worst insert-to-ready {worst} ms")
    assert not failures, failures[:5]
