"""Live diagnostics, alarms and the poll rate."""


def test_ddm_values_in_operating_range(ready):
    d = ready.cmd("ddm")
    assert -5 <= d["temp_c"] <= 75
    assert 3.13 <= d["vcc_v"] <= 3.47      # 3.3 V +/- 5%
    assert 1 <= d["bias_ma"] <= 15
    assert -10 <= d["tx_dbm"] <= 2
    assert d["rx_dbm"] is None or -25 <= d["rx_dbm"] <= 2
    assert d["age_ms"] <= 150             # never staler than one 100 ms poll plus slack


def test_poll_rate_is_10_hz(ready):
    ready.wait(2000)
    polls = ready.cmd("stats")["polls"]
    assert 18 <= polls <= 22, polls


def test_thresholds_are_ordered(ready):
    th = ready.cmd("thresholds")
    for name, (hi_alarm, lo_alarm, hi_warn, lo_warn) in th.items():
        assert lo_alarm <= lo_warn <= hi_warn <= hi_alarm, name


def test_no_alarms_on_a_healthy_link(ready):
    assert ready.cmd("alarms") == {"alarms": [], "warnings": []}


def test_fibre_pull_sets_and_clears_los(ready):
    ready.set_los(True)
    ready.wait(100)
    assert ready.cmd("ddm")["los"] is True
    ready.set_los(False)
    ready.wait(150)  # clears after 50 ms of stable light
    assert ready.cmd("ddm")["los"] is False
    kinds = [e["type"] for e in ready.cmd("events")["events"]]
    assert "LOS" in kinds and "LOS_CLEAR" in kinds
