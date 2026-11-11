"""Firmware readings against `ethtool -m` from the same module.

In CI the reference is tests_hil/fixtures/ethtool_lr_reference.txt: ethtool's
output format, computed with ethtool's formulas from the same EEPROM image the
simulator serves. On the bench pass a real capture with --ethtool.
"""
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "scripts"))
from ethtool_compare import compare, compare_hex, parse_ethtool, parse_ethtool_hex  # noqa: E402

FIX = pathlib.Path(__file__).parent / "fixtures"


@pytest.fixture
def ethtool_text(request, dut):
    path = request.config.getoption("--ethtool")
    if path:
        return pathlib.Path(path).read_text()
    if not dut.is_sim:
        pytest.skip("pass --ethtool <capture> on the bench")
    return (FIX / "ethtool_lr_reference.txt").read_text()


def test_readings_agree_with_ethtool(ready, ethtool_text):
    rows = compare(parse_ethtool(ethtool_text), ready.cmd("id"), ready.cmd("ddm"))
    for row in rows:
        print(row)
    assert len(rows) == 9
    assert all(ok for *_, ok in rows), [r for r in rows if not r[4]]


def test_power_matches_ethtool_to_the_hundredth(ready, ethtool_text):
    """Stricter than the 0.5 dB bench tolerance: with no drift between reads
    (the simulator) the two conversions should print the same number."""
    if not ready.is_sim:
        pytest.skip("live modules drift between the two reads")
    eth = parse_ethtool(ethtool_text)
    d = ready.cmd("ddm")
    assert d["tx_dbm"] == eth["tx_dbm"] and d["rx_dbm"] == eth["rx_dbm"], (d, eth)


def test_a0_bytes_match_ethtool_dump(ready):
    if not ready.is_sim:
        pytest.skip("pass a real hex capture to scripts/ethtool_compare.py --hex")
    eth = parse_ethtool_hex((FIX / "ethtool_lr_reference_a0.hex").read_text())
    assert compare_hex(eth, ready.cmd("raw a0 0 128")["hex"]) == []


def test_a_real_mismatch_is_caught(ready, ethtool_text):
    off_by_a_db = ethtool_text.replace("/ -2.40 dBm", "/ -3.40 dBm")
    rows = compare(parse_ethtool(off_by_a_db), ready.cmd("id"), ready.cmd("ddm"))
    assert [r[0] for r in rows if not r[4]] == ["tx_dbm"]
