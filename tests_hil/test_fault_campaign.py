"""Fault campaign: hundreds of injected bus faults through the shell.

Same commands on the board (test build) and in the simulator:
  inject nack <n>      the next n transfers NACK, like a module busy writing
  inject sda_low <ms>  the spare GPIO holds SDA low through a 220 ohm resistor

A fault counts as recovered when, after it, the module is still READY, the
diagnostics are fresh, and no power cycle was needed.
"""
import random

import pytest


def _recovered(dut):
    if dut.cmd("state")["state"] != "READY":
        return False
    return dut.cmd("ddm").get("age_ms", 10_000) <= 150


@pytest.mark.slow
def test_nack_burst_campaign(ready, request):
    n = request.config.getoption("--faults")
    rng = random.Random(1)  # same bursts every run
    recovered, injected = 0, 0
    for _ in range(n):
        burst = rng.randint(1, 3)  # up to 3: absorbed inside one transfer's 4 attempts
        injected += burst
        ready.ok(f"inject nack {burst}")
        ready.wait(250)
        recovered += _recovered(ready)
    s = ready.cmd("stats")
    print(f"\nNACK bursts: {recovered} of {n} recovered, {injected} NACKs injected, "
          f"{s['i2c_nack']} counted, {s['i2c_retries']} retries, {s['power_cycles']} power cycles")
    assert s["i2c_nack"] == injected
    assert s["power_cycles"] == 0
    assert recovered == n


@pytest.mark.slow
def test_stuck_sda_campaign(ready, request):
    n = request.config.getoption("--faults")
    recovered = 0
    for _ in range(n):
        ready.ok("inject sda_low 120")  # longer than one 100 ms poll period
        ready.wait(400)
        recovered += _recovered(ready)
    s = ready.cmd("stats")
    print(f"\nStuck SDA: {recovered} of {n} recovered, {s['bus_recoveries']} bus recoveries, "
          f"{s['power_cycles']} power cycles")
    assert s["bus_recoveries"] >= n
    assert recovered == n
