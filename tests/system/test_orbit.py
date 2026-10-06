#!/usr/bin/env python3
"""System test TC-14: one orbit without intervention (about 100 minutes).

From NOS3's deployment tip-off the OBC detumbles and points at the Sun on its own; the test then watches a full
orbit and checks it against 42's truth:
  - pointing in sunlight, from the truth Sun vector
  - eclipse detection: the OBC's (coarse sun sensors lose the Sun) against a cylindrical Earth shadow computed
    from 42's position and Sun direction, which the OBC never sees
  - reacquisition of the Sun after the eclipse
  - wheel momentum (momentum management keeps it bounded)
  - health: no fault, no unplanned mode change, no telemetry gap
The time series goes to orbit.csv in the log directory (the report plots it).

    sil/sil.sh 'sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&
                python3 tests/system/test_orbit.py'
"""
import csv
import math
import os
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_adcs import AdcsGround, vec, wait_mode, DEG  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

ORBIT_S = 5553.0        # 400 km circular (simulation time)
EARTH_R = 6378137.0
RW_INERTIA = 1.72e-5
RW_CAPACITY = 0.01082   # N m s
results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:44s} {detail}", flush=True)


def truth_eclipse(t):
    """Cylindrical Earth shadow from 42's inertial position and the Sun direction (body Sun vector rotated by the
    attitude quaternion; 42 convention: scalar last, qn rotates N into body axes)."""
    q, s_b, r = t["qn"], t["svb"], t["pos_n"]
    x, y, z, w = q
    # C (body <- N); s_n = C^T s_b
    c = [[w * w + x * x - y * y - z * z, 2 * (x * y + w * z), 2 * (x * z - w * y)],
         [2 * (x * y - w * z), w * w - x * x + y * y - z * z, 2 * (y * z + w * x)],
         [2 * (x * z + w * y), 2 * (y * z - w * x), w * w - x * x - y * y + z * z]]
    s_n = [sum(c[k][i] * s_b[k] for k in range(3)) for i in range(3)]
    n = math.sqrt(sum(v * v for v in s_n))
    s_n = [v / n for v in s_n]
    d = sum(r[i] * s_n[i] for i in range(3))
    perp = math.sqrt(max(0.0, sum(v * v for v in r) - d * d))
    return d < 0 and perp < EARTH_R


def edges(series, value):
    """Times at which a boolean series changes to value."""
    return [t for (t0, a), (t, b) in zip(series, series[1:]) if a != value and b == value]


def main():
    g = AdcsGround()
    rows = []
    try:
        return run(g, rows)
    finally:
        if rows:
            path = pathlib.Path(os.environ.get("SIL_LOG_DIR", ".")) / "orbit.csv"
            with open(path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
                w.writeheader()
                w.writerows(rows)
            print(f"  time series: {path} ({len(rows)} rows)")


def run(g, rows):
    g.wait_for("OBC_HK", 30.0)
    g.pump(3.0, until=lambda: g.truth is not None)
    if wait_mode(g, "SUN_POINT", 400.0) is None:
        check(False, "[TC-14.1] pointing in sunlight", "never reached SUN_POINT")
        return 1
    st = g.wait_for("ADCS_STATE", 600.0, lambda f: f["CONVERGED"] == 1)
    n_events = len(g.events)
    t0 = time.monotonic()
    hk_times, last_hk = [], g.counts.get("OBC_HK", 0)
    print(f"  converged; watching one orbit ({ORBIT_S / 60:.0f} min of simulation time)", flush=True)

    sim_elapsed = 0.0
    truth_t0 = None
    while sim_elapsed < ORBIT_S:
        g.pump(1.0)
        now = time.monotonic() - t0
        if g.counts.get("OBC_HK", 0) != last_hk:
            last_hk = g.counts["OBC_HK"]
            hk_times.append(now)
        st = g.latest["ADCS_STATE"][1]
        hk = g.latest["OBC_HK"][1]
        t = g.truth
        h = [abs(x) * RW_INERTIA for x in vec(st, "RW_SIM_SPEED")]
        rows.append({
            "t_s": round(now, 1), "mode": hk["MODE"], "err_truth_deg": round(g.truth_sun_error_deg(), 3),
            "err_obc_deg": round(st["POINTING_ERROR"] / DEG, 3) if st["POINTING_ERROR"] >= 0 else "",
            "sun_valid": st["SUN_VALID"], "eclipse_truth": int(truth_eclipse(t)),
            "rate_truth_dps": round(g.truth_rate_deg(), 4), "wheel_momentum_pct": round(max(h) / RW_CAPACITY * 100, 2),
            "batt_v": round(g.latest["EPS_SIM"][1]["BATT_V"], 3) if "EPS_SIM" in g.latest else ""})
        # Simulation time from the truth stream (the simulation runs a little slower than the wall clock)
        utc = t.get("utc")
        if utc is None:
            sim_elapsed = now
        else:
            truth_t0 = truth_t0 or utc
            sim_elapsed = (utc - truth_t0).total_seconds()
        if len(rows) % 600 == 0:
            print(f"  {now / 60:.0f} min: mode {hk['MODE']}, Sun angle {rows[-1]['err_truth_deg']:.2f} deg, eclipse "
                  f"{rows[-1]['eclipse_truth']}, wheel momentum {rows[-1]['wheel_momentum_pct']:.1f} %", flush=True)

    # ---- Evaluate ----
    ecl = [(r["t_s"], r["eclipse_truth"] == 1) for r in rows]
    lost = [(r["t_s"], r["sun_valid"] == 0) for r in rows]
    entries_t, exits_t = edges(ecl, True), edges(ecl, False)
    entries_o, exits_o = edges(lost, True), edges(lost, False)

    def match(truth_times, obc_times):
        return [min((abs(o - t) for o in obc_times), default=None) for t in truth_times]
    d_in, d_out = match(entries_t, entries_o), match(exits_t, exits_o)
    eclipse_ok = bool(entries_t) and all(d is not None and d < 30 for d in d_in + d_out)
    check(eclipse_ok, "[TC-14.2] eclipse detection",
          f"{len(entries_t)} eclipse entries and {len(exits_t)} exits in 42's geometry; OBC detected them within "
          f"{max([d for d in d_in + d_out if d is not None], default=float('nan')):.0f} s"
          if entries_t else "no eclipse in the orbit?")

    # Sunlit samples more than 5 minutes after acquisition (start of the watch, or an eclipse exit)
    def settled(r):
        since = min([r["t_s"] - x for x in [0.0] + exits_t if r["t_s"] >= x], default=r["t_s"])
        return r["eclipse_truth"] == 0 and since > 300
    sunlit = [r["err_truth_deg"] for r in rows if settled(r)]
    worst = max(sunlit, default=float("nan"))
    check(sunlit and worst < 2.0, "[TC-14.1] pointing in sunlight",
          f"worst {worst:.2f} deg over {len(sunlit)} s of settled sunlight (42 truth), median "
          f"{sorted(sunlit)[len(sunlit) // 2]:.2f} deg" if sunlit else "no settled sunlight")

    reacq = []
    for x in exits_t:
        after = [r for r in rows if r["t_s"] >= x and r["eclipse_truth"] == 0]
        t5 = next((r["t_s"] - x for r in after if r["err_truth_deg"] < 5.0), None)
        reacq.append(t5)
    check(reacq and all(t is not None and t < 300 for t in reacq), "[TC-14.3] reacquisition after eclipse",
          f"within 5 deg {', '.join(f'{t:.0f} s' for t in reacq if t is not None)} after eclipse exit"
          if reacq else "no eclipse exit in the orbit")

    peak_h = max(r["wheel_momentum_pct"] for r in rows)
    check(peak_h < 50.0, "[TC-14.4] wheel momentum bounded",
          f"peak {peak_h:.1f} % of capacity (wheel {RW_CAPACITY * 1000:.2f} mN m s)")

    # A fault is any event of WARNING severity or above (RF contact coming and going is INFO: normal operations)
    faults = [e[2] for e in g.events[n_events:] if e[1] >= icd.SEVERITY["WARNING"]]
    modes = sorted({r["mode"] for r in rows})
    gaps = [b - a for a, b in zip(hk_times, hk_times[1:])]
    check(not faults and modes == [icd.MODE["SUN_POINT"]] and max(gaps, default=0) < 5.0,
          "[TC-14.5] health over the orbit",
          f"{len(faults)} events of WARNING severity or above, {len(g.events) - n_events} events in all, modes {modes}, longest housekeeping gap {max(gaps, default=0):.1f} s, "
          f"{len(hk_times)} OBC_HK packets" + (f"; {faults[:3]}" if faults else ""))

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
