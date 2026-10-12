"""Bus faults, laser faults and power control."""


def test_nack_burst_is_retried_without_losing_ready(ready):
    ready.ok("inject nack 3")
    ready.wait(300)
    s = ready.cmd("stats")
    assert s["i2c_nack"] == 3 and s["i2c_retries"] >= 3
    assert ready.cmd("state")["state"] == "READY"
    assert ready.cmd("ddm")["age_ms"] <= 150


def test_sda_held_low_is_recovered(ready):
    ready.ok("inject sda_low 120")  # longer than one 100 ms poll, so a read meets it
    ready.wait(400)
    s = ready.cmd("stats")
    assert s["bus_recoveries"] >= 1
    assert s["power_cycles"] == 0  # cleared by bus recovery alone
    assert ready.cmd("state")["state"] == "READY"


def test_power_off_then_on_reidentifies(ready):
    ready.ok("power off")
    assert ready.cmd("state")["state"] == "OFF"
    ready.ok("power on")
    took = ready.wait_for_state("READY", 1000)
    assert took <= 400, took


def test_tx_disable_round_trip(ready):
    ready.ok("txdis on")
    ready.ok("txdis off")
    assert ready.cmd("state")["state"] == "READY"


def test_tx_fault_that_clears_is_recovered(ready):
    ready.tx_fault(2)
    ready.wait(1500)
    assert ready.cmd("state")["state"] == "READY"
    assert ready.cmd("stats")["fault_recoveries"] == 2


def test_tx_fault_that_stays_is_latched_with_laser_off(ready):
    ready.tx_fault(0)
    ready.wait(1500)
    assert ready.cmd("state")["state"] == "TX_FAULT"
    assert ready.cmd("stats")["fault_recoveries"] == 3


def test_pull_during_power_up_goes_absent(dut):
    if dut.cmd("state")["state"] != "ABSENT":
        dut.remove()
        dut.wait_for_state("ABSENT", 200)
    dut.insert()
    dut.wait(100)
    assert dut.cmd("state")["state"] == "POWERING"
    dut.remove()
    dut.wait_for_state("ABSENT", 50)
