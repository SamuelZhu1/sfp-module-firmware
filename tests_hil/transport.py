"""Two ways to reach the firmware shell, one JSON line in, one JSON line out.

SimTransport runs the host simulator (the real SDK and shell against a fake
module) and moves fake time with "sim wait". SerialTransport talks to the
board over its USB Serial/JTAG port and sleeps for real. Tests only call the
methods below, so the same test file runs in CI and on the bench.
"""
import json
import subprocess
import time


class ShellError(RuntimeError):
    pass


class _Base:
    def cmd(self, line):
        reply = json.loads(self._exchange(line))
        return reply

    def ok(self, line):
        reply = self.cmd(line)
        if reply.get("ok") is not True:
            raise ShellError(f"{line!r} -> {reply}")
        return reply

    def wait_for_state(self, want, timeout_ms, step_ms=10):
        waited = 0
        while waited <= timeout_ms:
            state = self.cmd("state")["state"]
            if state == want:
                return waited
            self.wait(step_ms)
            waited += step_ms
        raise AssertionError(f"state {state}, wanted {want} within {timeout_ms} ms")


class SimTransport(_Base):
    is_sim = True

    def __init__(self, binary, module="lr"):
        self.p = subprocess.Popen([binary, module], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def _exchange(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = self.p.stdout.readline()
        if not out:
            raise ShellError("simulator exited")
        return out

    def wait(self, ms):
        self.ok(f"sim wait {int(ms)}")

    # bench actions: the simulator does them itself
    def insert(self):
        self.ok("sim insert")

    def remove(self):
        self.ok("sim remove")

    def set_los(self, on):
        self.ok(f"sim los {'on' if on else 'off'}")

    def tx_fault(self, clears_after):
        self.ok(f"sim txfault {clears_after}")

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=5)


class SerialTransport(_Base):
    """Board on USB. Plugging, pulling and fibre actions need a person at the
    bench (or the relay fixture in docs/bringup.md), so those prompt."""
    is_sim = False

    def __init__(self, port, interactive=False):
        import serial  # pyserial, only needed on the bench
        self.s = serial.Serial(port, 115200, timeout=2)
        self.interactive = interactive
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def _exchange(self, line):
        self.s.write((line + "\n").encode())
        while True:
            raw = self.s.readline().decode(errors="replace").strip()
            if not raw:
                raise ShellError(f"no reply to {line!r}")
            if raw.startswith("{"):
                return raw  # skip any stray log line

    def wait(self, ms):
        time.sleep(ms / 1000)

    def _ask(self, what):
        import pytest
        if not self.interactive:
            pytest.skip(f"needs a person at the bench: {what} (run with --interactive)")
        input(f"\n>>> {what}, then press Enter ")

    def insert(self):
        self._ask("plug the module into the cage")

    def remove(self):
        self._ask("pull the module out of the cage")

    def set_los(self, on):
        self._ask("unplug the fibre" if on else "plug the fibre back in")

    def tx_fault(self, clears_after):
        import pytest
        pytest.skip("TX_FAULT is only forced in the simulator")

    def close(self):
        self.s.close()
