#!/usr/bin/env python3
"""System test TC-17: a restart in flight.

The automated form of the manual test that found NCR-020 (pulling the Pico's USB cable while it pointed at the
Sun). From NOS3's deployment tip-off the OBC detumbles and points at the Sun; then it is restarted (OBC_REBOOT:
on Linux a new process image, on the Pico a real reset whose USB port goes away and comes back), and:
  1. it reports the restart and comes back in SAFE
  2. it commands every actuator off: NOS3's wheel and magnetorquer simulators keep applying their last command,
     so anything left from before the restart would keep turning the spacecraft
  3. it waits in SAFE for the ground (design decision of 15 October 2026: after an unexplained restart, a person
     decides what happens next)
  4. it points at the Sun again when commanded
The actuator commands are read independently of the OBC's telemetry: the wheels' from NOS3's wheel simulators' logs,
the magnetorquers' from the bridge's log as they leave the OBC (the torquer simulator's log is written in batches,
so after the last command it can lag by seconds).

    sil/sil.sh 'sil/start_nodes.sh && sil/start_obc.sh && python3 tests/system/test_restart.py'
"""
import os
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_adcs import AdcsGround, wait_mode  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

LOG_DIR = pathlib.Path(os.environ.get("SIL_LOG_DIR", "."))
REBOOT_MAGIC = 0x0B0075ED
TUMBLE_DPS = 2.5
WHEEL = re.compile(r"SC\[0\]\.Whl\[(\d)\]\.Tcmd = (\S+)")
TORQUER = re.compile(r"TRQ_CMD torquer (\d) duty (-?\d+)")
results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:44s}  {detail}", flush=True)


class Actuators:
    """The actuator commands, from the logs: the last value for each actuator, and whether it was commanded at all
    since a mark. Wheels in N m, magnetorquers in 0.01 % of full duty."""
    FILES = [("wheel", WHEEL, f"generic-reactionwheel-sim{i}.log") for i in range(3)] + \
            [("torquer", TORQUER, "bridge.log")]

    def __init__(self):
        self.mark_at = {}

    def _read(self, since_mark):
        last, seen = {}, set()
        for kind, rx, name in self.FILES:
            path = LOG_DIR / name
            if not path.exists():
                continue
            data = path.read_text(errors="replace")
            start = self.mark_at.get(name, 0) if since_mark else 0
            for m in rx.finditer(data):
                key = f"{kind} {m.group(1)}"
                last[key] = float(m.group(2))
                if m.start() >= start:
                    seen.add(key)
        return last, seen

    def mark(self):
        for _, _, name in self.FILES:
            path = LOG_DIR / name
            self.mark_at[name] = path.stat().st_size if path.exists() else 0

    def last(self):
        return self._read(False)[0]

    def commanded_since_mark(self):
        return self._read(True)[1]


def fmt(values):
    return ", ".join(f"{k} {v:.3g}" for k, v in sorted(values.items()))


def main():
    g = AdcsGround()
    act = Actuators()
    if g.wait_for("OBC_HK", 30.0) is None:
        check(False, "[TC-17.1] restart reported", "no telemetry from the OBC")
        return 1

    # ---- To sun pointing, from NOS3's tip-off ----
    if wait_mode(g, "SUN_POINT", 400.0) is None or g.wait_for("ADCS_STATE", 300.0, lambda f: f["CONVERGED"] == 1) is None:
        check(False, "[TC-17.1] restart reported", "never converged in SUN_POINT before the restart")
        return 1
    g.pump(10.0)
    before = act.last()

    # ---- 1. Restart ----
    act.mark()
    g.send("OBC_REBOOT", MAGIC=REBOOT_MAGIC)
    t0 = time.monotonic()
    g.latest.pop("OBC_HK", None)
    g.pump(1.0)
    hk = g.wait_for("OBC_HK", 20.0, lambda f: f["RESET_CAUSE"] == icd.RESET_CAUSE["COMMAND"] and
                    f["UPTIME"] < time.monotonic() - t0 + 1)
    check(hk is not None and hk["MODE"] == icd.MODE["SAFE"], "[TC-17.1] restart reported",
          f"telemetry back {time.monotonic() - t0:.1f} s after OBC_REBOOT in SUN_POINT: reset cause "
          f"{hk['RESET_CAUSE']} (COMMAND), mode {hk['MODE']} (SAFE)" if hk else "no telemetry after the restart")
    if hk is None:
        return 1

    # ---- 2. Actuators off (counted from the restarted OBC's first telemetry: a Pico takes seconds to come back) ----
    t_back   = time.monotonic()
    deadline = t_back + 10.0
    while time.monotonic() < deadline:
        g.pump(0.5)
        after, seen = act.last(), act.commanded_since_mark()
        if len(seen) == 6 and all(v == 0.0 for v in after.values()):
            break
    after, seen = act.last(), act.commanded_since_mark()
    off = len(seen) == 6 and all(v == 0.0 for v in after.values())
    still = {k: v for k, v in after.items() if k not in seen or v != 0.0}
    check(off, "[TC-17.2] actuators off after the restart",
          f"commands before the restart: {fmt(before)}; after: all six commanded to 0, seen "
          f"{time.monotonic() - t_back:.1f} s after the OBC was back" if off else
          f"commands before the restart: {fmt(before)}; still applied after it: {fmt(still)}")

    # ---- 3. Waits in SAFE ----
    worst, modes, end = 0.0, set(), time.monotonic() + 60.0
    while time.monotonic() < end:
        g.pump(1.0)
        modes.add(g.latest["OBC_HK"][1]["MODE"])
        worst = max(worst, g.truth_rate_deg())
    check(modes == {icd.MODE["SAFE"]} and worst < TUMBLE_DPS, "[TC-17.3] waits in SAFE",
          f"60 s with no command: modes {sorted(modes)} (SAFE {icd.MODE['SAFE']}), body rate at most "
          f"{worst:.3f} deg/s (42 truth)")

    # ---- 4. Sun pointing on command ----
    g.send("OBC_SET_MODE", MODE=icd.MODE["SUN_POINT"])
    t1 = time.monotonic()
    g.wait_for("OBC_HK", 5.0, lambda f: f["MODE"] == icd.MODE["SUN_POINT"])
    g.latest.pop("ADCS_STATE", None)  # only a report from after the mode change counts
    st = g.wait_for("ADCS_STATE", 300.0, lambda f: f["CONVERGED"] == 1)
    err = g.truth_sun_error_deg()
    check(st is not None and err < 5.0, "[TC-17.4] sun pointing on command",
          f"converged {time.monotonic() - t1:.0f} s after OBC_SET_MODE SUN_POINT, Sun angle {err:.2f} deg (42 truth)"
          if st else "not converged within 5 min")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
