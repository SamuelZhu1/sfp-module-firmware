"""Insertion, identity and the EEPROM checksums."""


def test_insert_reaches_ready_within_t_init_budget(dut):
    dut.insert()
    dut.wait_for_state("READY", 1000)
    ready_ms = dut.cmd("stats")["last_ready_ms"]
    # 20 ms debounce + 300 ms SFF-8431 t_init + identity reads at 100 kHz
    assert 300 <= ready_ms <= 400, ready_ms


def test_insert_events_in_order(dut):
    dut.insert()
    dut.wait_for_state("READY", 1000)
    ev = dut.cmd("events")["events"]
    kinds = [e["type"] if e["type"] != "STATE" else e["arg"] for e in ev]
    assert kinds == ["MOD_PRESENT", "POWERING", "IDENTIFYING", "READY"], kinds
    t = [e["t_us"] for e in ev]
    assert t == sorted(t)
    assert t[2] - t[1] >= 300_000  # power to first read waits out t_init


def test_identity_is_sane(ready):
    id_ = ready.cmd("id")
    assert id_["vendor"].strip() and id_["part"].strip() and id_["serial"].strip()
    assert id_["wavelength_nm"] in (850, 1310, 1550)
    assert 1000 <= id_["bitrate_mbps"] <= 11000
    assert id_["ddm"] is True


def _sum8(hexstr):
    return sum(bytes.fromhex(hexstr)) & 0xFF


def test_a0_checksums_match_the_eeprom(ready):
    base = bytes.fromhex(ready.cmd("raw a0 0 64")["hex"])
    ext = bytes.fromhex(ready.cmd("raw a0 64 32")["hex"])
    assert sum(base[:63]) & 0xFF == base[63]
    assert sum(ext[:31]) & 0xFF == ext[31]
    assert base[0] == 0x03  # SFP/SFP+


def test_version_reports(dut):
    assert dut.cmd("version")["sdk"]


def test_unknown_command_is_an_error(dut):
    assert dut.cmd("frobnicate")["error"] == "unknown command"


def test_id_with_no_module_is_absent(dut):
    if dut.cmd("state")["state"] != "ABSENT":
        dut.remove()
        dut.wait_for_state("ABSENT", 200)
    assert dut.cmd("id") == {"error": "ABSENT"}
