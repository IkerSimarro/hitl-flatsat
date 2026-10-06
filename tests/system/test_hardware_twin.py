#!/usr/bin/env python3
"""System test TC-13: the FlatSat's own hardware, against the plant model's true values.

  1. mirroring: in SUN_POINT the physical wheel runs at 1/10 of simulated wheel 0
  2. wheel steps (TEST mode): settling and overshoot, from the plant's true wheel speed at 50 Hz
  3. power measurement: the EPS node's INA219 readings against the true rail voltages and currents, four loads
  4. OBC hang (process frozen) with the wheel running: the ADCS node removes motor drive on its own

The plant model's truth comes from the SIL harness (shared memory, sil_harness.h). The wheel steps are recorded
in wheel_steps.csv in the log directory, for the report. Runs in the SIL environment (about 8 minutes):

    sil/sil.sh 'sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&
                python3 tests/system/test_hardware_twin.py'
"""
import csv
import mmap
import os
import pathlib
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_obc_umbilical import Ground  # noqa: E402
from test_adcs import wait_mode  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

LOG_DIR = pathlib.Path(os.environ.get("SIL_LOG_DIR", "."))
RPM = 2 * 3.141592653589793 / 60
results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:46s} {detail}", flush=True)


class Plant:
    """The plant model's true state (firmware/hal/linux/sil_harness.h)."""

    def __init__(self):
        self.f = open("/dev/shm/flatsat-harness", "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)

    def duty(self):
        return struct.unpack_from("f", self.m, 8)[0]

    def rpm(self):
        return struct.unpack_from("f", self.m, 24)[0]

    def rails(self):
        """([V] x3, [mA] x3, battery V)"""
        return list(struct.unpack_from("3f", self.m, 32)), list(struct.unpack_from("3f", self.m, 44)), \
            struct.unpack_from("f", self.m, 56)[0]


def wheel(g, rpm):
    """Speed control at rpm (0 brakes to a stop)."""
    g.send("ADCS_PHYS_WHEEL_TEST", CTRL_MODE=icd.RW_CTRL_MODE["SPEED"], SETPOINT=int(rpm))


def wheel_off(g):
    """No drive: the wheel coasts."""
    g.send("ADCS_PHYS_WHEEL_TEST", CTRL_MODE=icd.RW_CTRL_MODE["OFF"], SETPOINT=0)


def main():
    g = Ground()
    plant = Plant()
    g.wait_for("OBC_HK", 20.0)

    # ---- 1. Mirroring in SUN_POINT (NOS3's tip-off: detumble, then sun pointing on their own) ----
    if wait_mode(g, "SUN_POINT", 300.0) is None:
        check(False, "[TC-13.3] physical wheel mirrors wheel 0", "never reached SUN_POINT")
        return 1
    g.pump(30.0)  # past the first wheel transients of the slew
    worst_cmd, worst_true, end = 0.0, 0.0, time.monotonic() + 30.0
    while time.monotonic() < end:
        st = g.wait_for("ADCS_STATE", 2.0)
        g.latest.pop("ADCS_STATE", None)
        if st is None:
            continue
        mirror = max(-550.0, min(550.0, st["RW_SIM_SPEED_0"] / RPM / 10.0))
        worst_cmd = max(worst_cmd, abs(st["PHYS_RW_CMD_SPEED"] / RPM - mirror))
        worst_true = max(worst_true, abs(plant.rpm() - st["PHYS_RW_CMD_SPEED"] / RPM))
    check(worst_cmd < 2.0 and worst_true < 20.0, "[TC-13.3] physical wheel mirrors wheel 0",
          f"command within {worst_cmd:.1f} rpm of wheel 0 / 10, true wheel speed within {worst_true:.1f} rpm of it "
          f"over 30 s (wheel 0 at {st['RW_SIM_SPEED_0'] / RPM:.0f} rpm)" if st else "no ADCS_STATE")

    # ---- 2. Wheel steps (TEST mode) ----
    g.send("OBC_SET_MODE", MODE=icd.MODE["TEST"])
    g.wait_for("OBC_HK", 3.0, lambda f: f["MODE"] == icd.MODE["TEST"])
    rows, worst_settle, worst_over, details = [], 0.0, 0.0, []
    t_start = time.monotonic()
    previous = 0.0
    for sp in (100, 300, -300, -100, 0):
        wheel(g, sp)
        t0 = time.monotonic()
        last_out, peak = 0.0, 0.0
        while time.monotonic() - t0 < 4.0:
            rpm = plant.rpm()
            t = time.monotonic() - t0
            rows.append({"t_s": round(time.monotonic() - t_start, 3), "setpoint_rpm": sp, "true_rpm": round(rpm, 2),
                         "duty": round(plant.duty(), 4)})
            if abs(rpm - sp) >= 20.0:
                last_out = t
            step = sp - previous
            if step and (rpm - sp) * (1 if step > 0 else -1) > peak:
                peak = (rpm - sp) * (1 if step > 0 else -1)
            g.pump(0.02)
        over = peak / abs(sp - previous) * 100 if sp != previous else 0.0
        worst_settle, worst_over = max(worst_settle, last_out), max(worst_over, over)
        details.append(f"{previous:+.0f}->{sp:+.0f}: {last_out:.2f} s, {over:.0f} %")
        previous = sp
    with open(LOG_DIR / "wheel_steps.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    check(worst_settle < 2.0 and worst_over < 10.0, "[TC-13.2] wheel steps",
          f"within 20 rpm after (worst) {worst_settle:.2f} s, overshoot {worst_over:.1f} %; " + "; ".join(details))

    # ---- 3. Power measurement against the plant's true values ----
    # A reading reaches the ground up to ~2.5 s after it was taken (10 Hz samples, 1 Hz reports, 1 Hz packets), so
    # it is compared with the true values over that window: its error is its distance from the range they covered
    WINDOW_S = 2.5
    worst_ma, worst_mv, cases = 0.0, 0.0, []
    for label, action in (("idle", lambda: wheel_off(g)), ("wheel 300 rpm", lambda: wheel(g, 300)),
                          ("wheel 500 rpm", lambda: wheel(g, 500)),
                          ("ADCS node in reset", lambda: (wheel_off(g), g.send("EPS_SWITCH", SWITCH_ID=0,
                                                                                 STATE=icd.SWITCH_STATE["OFF"])))):
        action()
        truth = []  # (time, volts, amps, battery)
        end = time.monotonic() + 6.0
        while time.monotonic() < end:
            truth.append((time.monotonic(),) + plant.rails())
            g.pump(0.05)
        g.latest.pop("EPS_REAL", None)
        while "EPS_REAL" not in g.latest:
            truth.append((time.monotonic(),) + plant.rails())
            g.pump(0.05)
        t_rx = time.monotonic()
        eps = g.latest["EPS_REAL"][1]
        recent = [x for x in truth if t_rx - WINDOW_S <= x[0] <= t_rx]

        def distance(value, samples):
            return max(0.0, value - max(samples), min(samples) - value)
        err_ma = max(distance(eps[f"RAIL_MA_{i}"], [x[2][i] for x in recent]) for i in range(3))
        err_mv = max([distance(eps[f"RAIL_MV_{i}"], [x[1][i] * 1000 for x in recent]) for i in range(3)] +
                     [distance(eps["BATT_MV"], [x[3] * 1000 for x in recent])])
        worst_ma, worst_mv = max(worst_ma, err_ma), max(worst_mv, err_mv)
        cases.append(f"{label}: rails {[eps[f'RAIL_MA_{i}'] for i in range(3)]} mA (true "
                     f"{[round(recent[-1][2][i], 1) for i in range(3)]})")
    g.send("EPS_SWITCH", SWITCH_ID=0, STATE=icd.SWITCH_STATE["ON"])
    check(worst_ma <= 2.0 and worst_mv <= 10.0, "[TC-13.1] rail measurement accuracy",
          f"worst error {worst_ma:.1f} mA, {worst_mv:.1f} mV against the true values over the reading's "
          f"{WINDOW_S} s delivery window; " + "; ".join(cases))

    # ---- 4. OBC hang with the wheel running ----
    g.wait_for("OBC_HK", 10.0, lambda f: f["NODE_ALIVE_MASK"] == 0x0E)
    wheel(g, 300)
    g.wait_for("ADCS_STATE", 6.0, lambda f: abs(f["PHYS_RW_MEAS_SPEED"] / RPM - 300) < 20)
    subprocess.run(["pkill", "-STOP", "-f", "firmware/build/obc"], check=True)
    t0, t_off = time.monotonic(), None
    while time.monotonic() - t0 < 3.0:
        if t_off is None and abs(plant.duty()) < 1e-6:
            t_off = time.monotonic() - t0
        time.sleep(0.01)
    subprocess.run(["pkill", "-CONT", "-f", "firmware/build/obc"], check=True)
    g.pump(5.0)
    g.send("OBC_SET_MODE", MODE=icd.MODE["SAFE"])
    check(t_off is not None and t_off < 1.5, "[TC-13.4] loss of commands (OBC frozen)",
          f"the ADCS node removed motor drive {t_off:.2f} s after the OBC stopped (1 s command timeout)"
          if t_off is not None else "motor drive still on after 3 s")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
