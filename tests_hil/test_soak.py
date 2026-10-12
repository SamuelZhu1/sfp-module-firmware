"""Hot-plug soak: plug, identify, read, pull, many times over."""
import pytest


@pytest.mark.slow
def test_hot_plug_soak(dut, request):
    cycles = request.config.getoption("--cycles")
    worst = 0
    failures = []
    for i in range(cycles):
        dut.insert()
        try:
            dut.wait_for_state("READY", 1000)
            assert dut.cmd("ddm")["vcc_v"] > 3.0
        except AssertionError as e:
            failures.append((i, str(e)))
        worst = max(worst, dut.cmd("stats")["last_ready_ms"])
        dut.remove()
        dut.wait_for_state("ABSENT", 100)
    print(f"\n{cycles} cycles, {len(failures)} failures, worst insert-to-ready {worst} ms")
    assert not failures, failures[:5]
