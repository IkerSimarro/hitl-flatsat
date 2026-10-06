#!/usr/bin/env python3
"""System test: a real pass, from 42's orbit, over NASA's Wallops ground station (ICD 7.4).

From the default start epoch the spacecraft rises over Wallops (37.94 N, 75.47 W) about 4 minutes in: a
6.5-minute pass reaching ~62 deg. The test checks the ground station's prediction against the pass as it happens,
store-and-forward across AOS, the link getting stronger towards culmination and LOS on both ends. It records the
pass (elevation, range, RSSI, SNR, what arrived) in rf_pass.csv in the log directory. About 12 minutes:

    SIL_GS_ARGS="--lat 37.9402 --lon -75.4664 --alt 10 --seed 1" sil/sil.sh 'sil/start_nodes.sh &&
        (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) && python3 tests/system/test_rf_pass.py'
"""
import csv
import os
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_rf_link import RfGround, check, results  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402


def main():
    g = RfGround()
    rows = []
    try:
        return run(g, rows)
    finally:
        if rows:
            path = pathlib.Path(os.environ.get("SIL_LOG_DIR", ".")) / "rf_pass.csv"
            with open(path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
                w.writeheader()
                w.writerows(rows)
            print(f"  pass record: {path} ({len(rows)} rows)")


def record(g, rows, t0):
    gs = g.gs()
    rows.append({"t_s": round(time.monotonic() - t0, 1), "contact": gs.get("CONTACT"),
                 "elevation_deg": round(gs.get("ELEVATION", 0), 2), "azimuth_deg": round(gs.get("AZIMUTH", 0), 1),
                 "range_km": round(gs.get("RANGE", 0), 1), "rssi_dbm": gs.get("LAST_RSSI"),
                 "snr_db": gs.get("LAST_SNR", 0) / 4, "down_frames": gs.get("DOWN_FRAMES"),
                 "down_lost": gs.get("DOWN_LOST"), "beacons": g.rf_counts.get("BEACON", 0),
                 "obc_contact": g.latest["COMMS_STATS"][1]["CONTACT"] if "COMMS_STATS" in g.latest else None})


def run(g, rows):
    t0 = time.monotonic()
    g.wait_for("OBC_HK", 20.0)
    g.send("OBC_SET_AUTO_MODES", STATE=icd.SWITCH_STATE["OFF"])

    # ---- 1. The prediction, before the pass ----
    gs = g.rf_wait("GS_STATUS", 10.0, lambda f: f["NEXT_AOS"] not in (0, 0xFFFFFFFF))
    if gs is None:
        check(False, "[TC-10.1] pass predicted", "no prediction")
        return 1
    # In simulation time (GS_STATUS time stamps): the simulation can run a few % slower than the wall clock
    aos_pred = g.rf_time["GS_STATUS"] + gs["NEXT_AOS"]
    los_pred = aos_pred + gs["NEXT_PASS_DURATION"]
    max_pred = gs["NEXT_MAX_ELEVATION"] / 10
    wall_deadline = time.monotonic() + gs["NEXT_AOS"] * 1.5 + 60
    check(60 < gs["NEXT_AOS"] < 600 and 200 < gs["NEXT_PASS_DURATION"] < 600 and max_pred > 30,
          "[TC-10.1] pass predicted from 42's truth", f"AOS in {gs['NEXT_AOS']} s, {gs['NEXT_PASS_DURATION']} s long, "
          f"max elevation {max_pred:.1f} deg")

    # Waiting for the pass: a telecommand at the ground and an event on board
    g.rf_send("COMMS_NOOP")
    g.send("OBC_NOOP")

    # ---- 2. AOS ----
    while not g.gs().get("CONTACT") and time.monotonic() < wall_deadline:
        g.pump(1.0)
        record(g, rows, t0)
    aos = g.rf_time["GS_STATUS"]
    el_aos = g.gs()["ELEVATION"]
    check(g.gs().get("CONTACT") == 1 and abs(aos - aos_pred) < 15, "[TC-10.2] AOS when predicted",
          f"{aos - aos_pred:+d} s from the prediction (simulation time, 1 s resolution), elevation {el_aos:.1f} deg, "
          f"range {g.gs()['RANGE']:.0f} km")

    # ---- 3. The pass ----
    first_rssi, max_el, rssi_at_max = None, -90.0, None
    n_ev = len(g.rf_events)
    wall_deadline = time.monotonic() + (los_pred - aos) * 1.5 + 60
    while g.gs().get("CONTACT") and time.monotonic() < wall_deadline:
        g.pump(1.0)
        record(g, rows, t0)
        gs = g.gs()
        if gs["LAST_RSSI"] and first_rssi is None:
            first_rssi = gs["LAST_RSSI"]
        if gs["ELEVATION"] > max_el:
            max_el, rssi_at_max = gs["ELEVATION"], gs["LAST_RSSI"]
    los = g.rf_time["GS_STATUS"]
    los_wall = time.monotonic()
    gs = g.gs()
    check(any("COMMS NOOP received via RF" in e for e in g.rf_events) and
          any(e.startswith("NOOP received") for e in g.rf_events[:n_ev] + g.rf_events[n_ev:]),
          "[TC-10.3] store and forward across AOS", "the telecommand queued before AOS was executed and the event stored "
          "before AOS came down")
    check(g.rf_counts.get("BEACON", 0) >= 20 and abs(max_el - max_pred) < 2.0 and first_rssi is not None and
          rssi_at_max is not None and rssi_at_max >= first_rssi + 5,
          "[TC-10.4] pass: beacons, culmination, link budget",
          f"{g.rf_counts.get('BEACON', 0)} beacons; max elevation {max_el:.1f} deg (predicted {max_pred:.1f}); "
          f"RSSI {first_rssi} dBm after AOS, {rssi_at_max} dBm at culmination; {gs['DOWN_FRAMES']} frames "
          f"delivered, {gs['DOWN_LOST']} lost")
    check(abs(los - los_pred) < 15 and abs((los - aos) - (los_pred - aos_pred)) < 15, "[TC-10.5] LOS when predicted",
          f"{los - los_pred:+d} s from the prediction; pass lasted {los - aos} s (predicted {los_pred - aos_pred} s)")

    # ---- 4. LOS on board ----
    st = g.wait_for("COMMS_STATS", 60.0, lambda f: f["CONTACT"] == 0)
    check(st is not None, "[TC-10.6] LOS noticed on board", f"{time.monotonic() - los_wall:.0f} s after LOS")
    record(g, rows, t0)

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
