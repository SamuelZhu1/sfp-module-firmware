import os
import pathlib

import pytest

from transport import SerialTransport, SimTransport

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_SIM = ROOT / "components/sfp/test_host/build/sfp_sim"


def pytest_addoption(parser):
    parser.addoption("--port", help="board serial port, e.g. /dev/ttyACM0; omit to use the simulator")
    parser.addoption("--sim", default=str(DEFAULT_SIM), help="path to the simulator binary")
    parser.addoption("--interactive", action="store_true", help="prompt for bench actions")
    parser.addoption("--cycles", type=int, default=200, help="hot-plug cycles in the soak test")
    parser.addoption("--power-cycles", type=int, default=200, help="load switch cycles in the soak test")
    parser.addoption("--faults", type=int, default=200, help="faults of each kind in the campaign")
    parser.addoption("--ethtool", help="`ethtool -m <if>` output captured from the same module")


@pytest.fixture
def dut(request):
    port = request.config.getoption("--port")
    if port:
        t = SerialTransport(port, request.config.getoption("--interactive"))
    else:
        sim = request.config.getoption("--sim")
        if not os.path.exists(sim):
            pytest.exit(f"{sim} missing: run `make -C components/sfp/test_host sim`")
        t = SimTransport(sim, getattr(request, "param", "lr"))
    t.ok("stats clear")
    yield t
    t.close()


@pytest.fixture
def ready(dut):
    """A module in the cage and identified."""
    if dut.cmd("state")["state"] == "ABSENT":
        dut.insert()
    dut.wait_for_state("READY", 1000)
    dut.cmd("events")  # drain the insertion events
    dut.ok("stats clear")
    return dut
