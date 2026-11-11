#!/usr/bin/env python3
"""Cross-check the firmware's readings against Linux `ethtool -m`.

Put the same module in a NIC first and capture what the kernel reads:

    ethtool -m eth0 > lr_ethtool.txt
    ethtool -m eth0 raw on hex on > lr_ethtool_a0.hex      # optional, A0h bytes

Then move it to the cage on the board and compare:

    scripts/ethtool_compare.py lr_ethtool.txt --port /dev/ttyACM0 [--hex lr_ethtool_a0.hex]

Both sides read the same A2h registers, so differences should only come from
unit conversion and from the module's values drifting between the two reads
(temperature and power move a little as it warms up). Exit code 1 if any field
is outside its tolerance.
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests_hil"))

# field: (tolerance, unit). Temperature and bias allow for drift between reads.
TOLERANCE = {
    "temp_c": (2.0, "C"),
    "vcc_v": (0.02, "V"),
    "bias_ma": (0.5, "mA"),
    "tx_dbm": (0.5, "dB"),
    "rx_dbm": (0.5, "dB"),
}

_PATTERNS = {
    "temp_c": r"Module temperature\s*:\s*(-?[\d.]+) degrees C",
    "vcc_v": r"Module voltage\s*:\s*([\d.]+) V",
    "bias_ma": r"Laser bias current\s*:\s*([\d.]+) mA",
    "tx_dbm": r"Laser output power\s*:\s*[\d.]+ mW / (-?[\d.]+|-inf) dBm",
    "rx_dbm": r"Receiver signal (?:average optical power|OMA)\s*:\s*[\d.]+ mW / (-?[\d.]+|-inf) dBm",
    "vendor": r"Vendor name\s*:\s*(.+?)\s*$",
    "part": r"Vendor PN\s*:\s*(.+?)\s*$",
    "serial": r"Vendor SN\s*:\s*(.+?)\s*$",
    "wavelength_nm": r"Laser wavelength\s*:\s*(\d+)\s*nm",
}


def parse_ethtool(text):
    """Pull the fields we compare out of `ethtool -m` text output."""
    out = {}
    for key, pat in _PATTERNS.items():
        m = re.search(pat, text, re.MULTILINE)
        if not m:
            continue
        v = m.group(1)
        if key in TOLERANCE:
            out[key] = None if v == "-inf" else float(v)
        elif key == "wavelength_nm":
            out[key] = int(v)
        else:
            out[key] = v
    return out


def parse_ethtool_hex(text):
    """`ethtool -m <if> raw on hex on` dump -> bytes (A0h first)."""
    data = bytearray()
    for line in text.splitlines():
        m = re.match(r"\s*0x[0-9a-fA-F]+:\s*((?:[0-9a-fA-F]{2}\s*)+)", line)
        if m:
            data += bytes.fromhex(m.group(1).replace(" ", ""))
    return bytes(data)


def compare(eth, fw_id, fw_ddm):
    """Rows of (field, ethtool, firmware, diff, ok)."""
    rows = []
    for key in ("vendor", "part", "serial", "wavelength_nm"):
        if key in eth:
            rows.append((key, eth[key], fw_id.get(key), "", eth[key] == fw_id.get(key)))
    for key, (tol, unit) in TOLERANCE.items():
        if key not in eth:
            continue
        a, b = eth[key], fw_ddm.get(key)
        if a is None or b is None:  # no light on one side
            rows.append((key, a, b, "", a is None and b is None))
            continue
        diff = b - a
        rows.append((key, a, b, f"{diff:+.2f} {unit}", abs(diff) <= tol))
    return rows


def compare_hex(eth_bytes, fw_hex, n=128):
    """Byte offsets where the A0h dumps differ."""
    fw = bytes.fromhex(fw_hex)
    return [i for i in range(min(n, len(eth_bytes), len(fw))) if eth_bytes[i] != fw[i]]


def print_rows(rows):
    print(f"{'field':<14}{'ethtool':>18}{'firmware':>18}{'diff':>12}  ok")
    for key, a, b, diff, ok in rows:
        print(f"{key:<14}{str(a):>18}{str(b):>18}{diff:>12}  {'yes' if ok else 'NO'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ethtool_txt", help="saved `ethtool -m <if>` output")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="board serial port")
    src.add_argument("--sim", help="simulator binary (for testing this script)")
    ap.add_argument("--hex", help="saved `ethtool -m <if> raw on hex on` output, to diff A0h bytes")
    args = ap.parse_args()

    from transport import SerialTransport, SimTransport
    dut = SerialTransport(args.port) if args.port else SimTransport(args.sim)
    if args.sim:
        dut.insert()
    dut.wait_for_state("READY", 1000)

    eth = parse_ethtool(pathlib.Path(args.ethtool_txt).read_text())
    rows = compare(eth, dut.cmd("id"), dut.cmd("ddm"))
    print_rows(rows)
    bad = [r for r in rows if not r[4]]

    if args.hex:
        diffs = compare_hex(parse_ethtool_hex(pathlib.Path(args.hex).read_text()),
                            dut.cmd("raw a0 0 128")["hex"])
        print(f"A0h bytes 0-127: {len(diffs)} differ" + (f" at {diffs[:10]}" if diffs else ""))
        bad += diffs
    dut.close()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
