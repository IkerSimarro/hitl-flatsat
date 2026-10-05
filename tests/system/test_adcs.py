#!/usr/bin/env python3
"""System test: attitude control and automatic modes, closed loop with 42, checked against 42's truth.

From a tumbling start the OBC must detumble on its own (B-dot, magnetorquers), hand over to sun pointing
(reaction wheels) and converge; then fault and low-battery responses. The OBC's own telemetry is never taken
on trust: rates and pointing are checked against 42's truth stream. Runs in the SIL environment (8-10 min):

    SIL_INIT_RATES="2 -3 4" sil/sil.sh 'sil/start_nodes.sh &&
        (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) && python3 tests/system/test_adcs.py'
"""
import csv
import math
import os
import pathlib
import select
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_obc_umbilical import Ground  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "ground"))
import flatsat_icd as icd  # noqa: E402

DEG = math.pi / 180
TRUTH_PORT = 5112  # the SIL truth relay's port for tests (ICD 3.6)
TRUTH_FIELDS = [("pos_n", 3), ("vel_n", 3), ("svb", 3), ("bvb", 3), ("hvb", 3), ("wn", 3), ("qn", 4),
                ("pos_w", 3), ("vel_w", 3), ("acc_b", 3), ("gyro_b", 3), ("rw_h", 3)]

results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:44s} {detail}", flush=True)


def norm(v):
    return math.sqrt(sum(x * x for x in v))


def vec(fields, name):
    return [fields[f"{name}_{i}"] for i in range(3)]


class AdcsGround(Ground):
    """Ground with 42's truth stream alongside the umbilical telemetry."""

    def __init__(self):
        super().__init__()
        self.truth_rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.truth_rx.bind(("0.0.0.0", TRUTH_PORT))
        self.truth = None
        self.t0 = time.monotonic()
        self.rows = []  # one per ADCS_STATE, with 42 truth alongside: the test's evidence

    def _read_truth(self):
        d = self.truth_rx.recv(1024)
        if struct.unpack(">h", d[:2])[0] == 0:
            return  # 42 not started yet
        t, off = {}, 20
        for name, n in TRUTH_FIELDS:
            t[name] = struct.unpack(f">{n}d", d[off:off + 8 * n])
            off += 8 * n
        self.truth = t

    def pump(self, seconds, until=None):
        deadline = time.monotonic() + seconds
        while until is None or not until():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            ready, _, _ = select.select([self.rx, self.truth_rx], [], [], remaining)
            if self.truth_rx in ready:
                self._read_truth()
            if self.rx in ready:
                n = self.counts.get("ADCS_STATE", 0)
                super().pump(0.001)
                if self.counts.get("ADCS_STATE", 0) != n and self.truth is not None:
                    self._record()

    def _record(self):
        st = self.latest["ADCS_STATE"][1]
        self.rows.append({
            "t_s": round(time.monotonic() - self.t0, 2), "mode": self.mode(), "adcs_mode": st["ADCS_MODE"],
            "sun_valid": st["SUN_VALID"], "converged": st["CONVERGED"],
            "err_truth_deg": round(self.truth_sun_error_deg(), 3),
            "err_obc_deg": round(st["POINTING_ERROR"] / DEG, 3) if st["POINTING_ERROR"] >= 0 else "",
            "rate_truth_dps": round(self.truth_rate_deg(), 4), "rate_obc_dps": round(norm(vec(st, "RATE_EST")) / DEG, 4),
            **{f"rw_torque_{i}_mNm": round(x * 1e3, 5) for i, x in enumerate(vec(st, "RW_CMD_TORQUE"))},
            **{f"rw_speed_{i}_rads": round(x, 2) for i, x in enumerate(vec(st, "RW_SIM_SPEED"))},
            **{f"trq_duty_{i}": x for i, x in enumerate(vec(st, "TRQ_DUTY"))},
            "phys_rw_cmd_rads": round(st["PHYS_RW_CMD_SPEED"], 2), "phys_rw_meas_rads": round(st["PHYS_RW_MEAS_SPEED"], 2),
        })

    def save(self, path):
        if self.rows:
            with open(path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=list(self.rows[0].keys()))
                w.writeheader()
                w.writerows(self.rows)
            print(f"  time series: {path} ({len(self.rows)} rows)")

    def truth_rate_deg(self):
        return norm(self.truth["wn"]) / DEG

    def truth_sun_error_deg(self):
        """Angle between body +X and the Sun, from 42's truth Sun vector in the body frame."""
        s = self.truth["svb"]
        return math.acos(max(-1.0, min(1.0, s[0] / norm(s)))) / DEG

    def mode(self):
        return self.latest["OBC_HK"][1]["MODE"] if "OBC_HK" in self.latest else None


def sim_command(target, command):
    subprocess.run([sys.executable, str(ROOT / "sil" / "sim_cmd.py"), target, command], check=True,
                   stdout=subprocess.DEVNULL)


def event_seen(g, text, since):
    return any(text in e[2] for e in g.events[since:])


def wait_mode(g, mode, timeout):
    return g.wait_for("OBC_HK", timeout, lambda f: f["MODE"] == icd.MODE[mode])


def main():
    g = AdcsGround()
    try:
        return run(g)
    finally:
        g.save(pathlib.Path(os.environ.get("SIL_LOG_DIR", ".")) / "adcs_timeseries.csv")


def run(g):
    g.wait_for("OBC_HK", 30.0)
    g.pump(3.0, until=lambda: g.truth is not None)
    w0 = g.truth_rate_deg()
    print(f"  start: 42 truth body rate {w0:.2f} deg/s, mode {g.mode()}", flush=True)

    # ---- 1. SAFE -> DETUMBLE on its own ----
    t0 = time.monotonic()
    hk = wait_mode(g, "DETUMBLE", 60.0)
    st = g.wait_for("ADCS_STATE", 5.0, lambda f: f["ADCS_MODE"] == icd.ADCS_MODE["BDOT"] and
                    any(d != 0 for d in vec(f, "TRQ_DUTY")))
    check(hk is not None and hk["MODE_REASON"] == icd.MODE_REASON["AUTO_RATES_HIGH"] and st is not None,
          "tumbling detected: SAFE -> DETUMBLE (auto)",
          f"after {time.monotonic() - t0:.0f} s at {w0:.1f} deg/s, torquer duty {vec(st, 'TRQ_DUTY') if st else '?'}"
          f" (0.01 %)")

    # ---- 2. Detumble, then DETUMBLE -> SUN_POINT on its own ----
    t_detumble = time.monotonic()
    hk = wait_mode(g, "SUN_POINT", 900.0)
    w_handover = g.truth_rate_deg()
    check(hk is not None and hk["MODE_REASON"] == icd.MODE_REASON["AUTO_CONVERGED"] and w_handover < 2.2,
          "B-dot detumble: DETUMBLE -> SUN_POINT (auto)",
          f"{w0:.2f} -> {w_handover:.2f} deg/s (42 truth) in {time.monotonic() - t_detumble:.0f} s")

    # ---- 3. Sun pointing converges (in sunlight) ----
    t_point = time.monotonic()
    st = g.wait_for("ADCS_STATE", 600.0, lambda f: f["CONVERGED"] == 1)
    if st:
        g.pump(1.0)
        err_truth = g.truth_sun_error_deg()
        err_obc = st["POINTING_ERROR"] / DEG
        check(err_truth < 5.0 and abs(err_truth - err_obc) < 3.0, "sun pointing converged (42 truth)",
              f"after {time.monotonic() - t_point:.0f} s: truth {err_truth:.2f} deg, OBC estimate {err_obc:.2f} deg, "
              f"rate {g.truth_rate_deg():.3f} deg/s")
    else:
        check(False, "sun pointing converged (42 truth)", f"no convergence, sun valid "
              f"{g.latest['ADCS_STATE'][1]['SUN_VALID']}, truth error {g.truth_sun_error_deg():.1f} deg")

    # ---- 4. Pointing holds; wheels within limits; the physical wheel mirrors wheel 0 ----
    worst, torque_peak, sun_lost, end = 0.0, 0.0, 0, time.monotonic() + 30.0
    while time.monotonic() < end:
        g.pump(1.0)
        worst = max(worst, g.truth_sun_error_deg())
        torque_peak = max([torque_peak] + [abs(x) for x in vec(g.latest["ADCS_STATE"][1], "RW_CMD_TORQUE")])
        sun_lost += g.latest["ADCS_STATE"][1]["SUN_VALID"] == 0
    st = g.latest["ADCS_STATE"][1]
    check(worst < 5.0 and torque_peak <= 1e-3 + 1e-9, "pointing held for 30 s, wheel torque in limits",
          f"worst {worst:.2f} deg (truth), peak wheel torque {torque_peak * 1e3:.3f} mN m, Sun invalid {sun_lost} s, "
          f"sim wheel speeds {[round(x, 1) for x in vec(st, 'RW_SIM_SPEED')]} rad/s, physical wheel "
          f"{st['PHYS_RW_MEAS_SPEED']:.1f} rad/s")

    # ---- 5. FDIR: IMU failure in SUN_POINT -> SAFE; no automatic restart while rates are low ----
    n = len(g.events)
    sim_command("imu-command", "DISABLE")
    t_fail = time.monotonic()
    hk = wait_mode(g, "SAFE", 10.0)
    t_safe = time.monotonic() - t_fail
    sim_command("imu-command", "ENABLE")
    g.pump(15.0)
    check(hk is not None and hk["MODE_REASON"] == icd.MODE_REASON["FAULT"] and event_seen(g, "IMU failed in", n)
          and g.mode() == icd.MODE["SAFE"] and event_seen(g, "IMU recovered", n),
          "IMU failure in SUN_POINT: SAFE, stays there", f"SAFE {t_safe:.1f} s after the IMU stopped, IMU recovered, "
          f"still SAFE 15 s later at {g.truth_rate_deg():.2f} deg/s")

    # ---- 6. Low simulated battery -> LOW_POWER, reduced telemetry; recovery -> SAFE ----
    sim_command("eps-command", "STATE_OF_CHARGE=20")
    hk = wait_mode(g, "LOW_POWER", 20.0)
    before = g.counts.get("ADCS_STATE", 0)
    g.pump(10.0)
    slowed = g.counts.get("ADCS_STATE", 0) - before
    check(hk is not None and hk["MODE_REASON"] == icd.MODE_REASON["LOW_BATTERY"] and slowed <= 2,
          "battery 20 %: LOW_POWER, telemetry reduced",
          f"{slowed} ADCS_STATE packets in 10 s (1 Hz normally), battery {g.latest['EPS_SIM'][1]['BATT_V']:.2f} V")
    sim_command("eps-command", "STATE_OF_CHARGE=60")
    hk = wait_mode(g, "SAFE", 30.0)
    check(hk is not None and hk["MODE_REASON"] == icd.MODE_REASON["BATTERY_RECOVERED"],
          "battery 60 %: back to SAFE (auto)", "reason BATTERY_RECOVERED")

    # ---- 7. Commanding: invalid gains rejected, automatic modes off ----
    rejects = g.latest["OBC_HK"][1]["CMD_REJECT_COUNT"]
    g.send("ADCS_SET_BDOT_GAIN", GAIN=float("nan"))
    g.send("ADCS_SET_SUN_GAINS", KP=-1.0, KD=0.28)
    hk = g.wait_for("OBC_HK", 5.0, lambda f: f["CMD_REJECT_COUNT"] >= rejects + 2)
    g.send("OBC_SET_AUTO_MODES", STATE=icd.SWITCH_STATE["OFF"])
    st = g.wait_for("ADCS_STATE", 5.0, lambda f: f["AUTO_MODES"] == 0)
    check(hk is not None and st is not None, "NaN and negative gains rejected; auto modes off",
          f"reject count {rejects} -> {g.latest['OBC_HK'][1]['CMD_REJECT_COUNT']}, AUTO_MODES "
          f"{g.latest['ADCS_STATE'][1]['AUTO_MODES']}")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
