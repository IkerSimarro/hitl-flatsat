#!/usr/bin/env python3
"""System test TC-15: the FlatSat's attitude control against NASA's reference ADCS (informative benchmark).

The same scenario is flown twice from the same 5.4 deg/s tumble, measured the same way from 42's truth:
  --fsw flatsat   the FlatSat OBC, on its own: automatic DETUMBLE, then SUN_POINT
  --fsw cfs       NOS3's flight software: cFS with the generic ADCS app (NASA's reference implementation). Its
                  device apps are enabled and it is commanded to B-dot, then to sun-safe pointing, by the same
                  hand-over rule the FlatSat uses (below 2 deg/s for 20 s)
  --compare DIR   reads both runs' metrics and prints the TC-15 results

The metrics go to reference_<fsw>.json in the log directory. tests/run_all.sh runs all three (stage
sil:reference). The cFS run needs the HIL bridge off and cFS's container settings:

    SIL_INIT_RATES="2 -3 4" SIL_NO_BRIDGE=1 SIL_DOCKER_ARGS="--sysctl fs.mqueue.msg_max=10000 --ulimit rtprio=99
        --cap-add=sys_nice" sil/sil.sh 'python3 tests/system/test_reference_adcs.py --fsw cfs'
"""
import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_adcs import AdcsGround, wait_mode  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
CFS_DIR = ROOT / "nos3/fsw/build/exe/cpu1"
CI_LAB = ("127.0.0.1", 5012)
HANDOVER_DPS, HANDOVER_S = 2.0, 20.0
BUDGET_S = 900.0

# NOS3 cFS command IDs (components/*/fsw/cfs/platform_inc/*_msgids.h, src/*_msg.h)
ADCS_MID, ADCS_SET_MODE = 0x1940, 2
BDOT_MODE, SUNSAFE_MODE = 1, 2
ENABLES = [(0x1925, 2, b""), (0x192A, 2, b""), (0x1910, 2, b""), (0x1920, 2, b""), (0x193A, 2, b""),
           (0x1935, 2, b""), (0x1870, 2, b"")] + [(0x1992, 4, struct.pack("<Bh", w, 0)) for w in range(3)]


def cfs_command(sock, mid, fc, payload=b""):
    """A cFS command packet (CCSDS v1 primary header, function code and XOR checksum, as in the FlatSat ICD 3.2)."""
    pkt = bytearray(struct.pack(">HHHBB", mid, 0xC000, 2 + len(payload) - 1, fc, 0) + payload)
    x = 0
    for b in pkt:
        x ^= b
    pkt[7] = x ^ 0xFF
    sock.sendto(bytes(pkt), CI_LAB)


def measure(g, start_detumble, command_sunpoint, log):
    """Common measurement: detumble time, sun pointing time, steady-state error, from 42's truth."""
    m = {"rate_at_start_dps": round(g.truth_rate_deg(), 2)}
    history = g.history = []
    t0 = start_detumble()
    m["rate_at_command_dps"] = round(g.truth_rate_deg(), 2)
    low_since = None
    while time.monotonic() - t0 < BUDGET_S:
        g.pump(0.5)
        rate = g.truth_rate_deg()
        history.append({"t_s": round(time.monotonic() - t0, 1), "phase": "detumble", "rate_dps": round(rate, 3),
                        "sun_deg": round(g.truth_sun_error_deg(), 2)})
        if rate < HANDOVER_DPS:
            low_since = low_since or time.monotonic()
            if "detumble_s" not in m:
                m["detumble_s"] = round(time.monotonic() - t0, 1)
                m["rate_at_detumble_dps"] = round(rate, 2)
            if time.monotonic() - low_since >= HANDOVER_S:
                break
        else:
            low_since = None
    t1 = command_sunpoint()
    errors = []
    while time.monotonic() - t1 < BUDGET_S:
        g.pump(1.0)
        err = g.truth_sun_error_deg()
        errors.append((time.monotonic() - t1, err))
        history.append({"t_s": round(time.monotonic() - t0, 1), "phase": "sun", "rate_dps": round(g.truth_rate_deg(), 3),
                        "sun_deg": round(err, 2)})
        if "point_5deg_s" not in m and err < 5.0:
            m["point_5deg_s"] = round(time.monotonic() - t1, 1)
        if "point_5deg_s" in m and time.monotonic() - t1 > m["point_5deg_s"] + 180:
            break
    tail = sorted(e for t, e in errors if t > errors[-1][0] - 60)
    m["steady_error_deg"] = round(tail[len(tail) // 2], 3) if tail else None
    m["start_angle_deg"] = round(errors[0][1], 1) if errors else None
    log(f"  metrics: {m}")
    return m


def run_flatsat(g):
    g.wait_for("OBC_HK", 30.0)

    def detumble():
        wait_mode(g, "DETUMBLE", 60.0)
        return time.monotonic()

    def sunpoint():
        wait_mode(g, "SUN_POINT", 60.0)  # the OBC hands over on its own, by the same rule
        return time.monotonic()
    return measure(g, detumble, sunpoint, print)


def run_cfs(g):
    log = open(os.path.join(os.environ.get("SIL_LOG_DIR", "/tmp"), "cfs.log"), "w")
    cfs = subprocess.Popen(["./core-cpu1", "-R", "PO"], cwd=CFS_DIR, stdout=log, stderr=subprocess.STDOUT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        time.sleep(12.0)  # cFS start-up: apps loaded, CI_LAB listening
        for mid, fc, payload in ENABLES:
            cfs_command(sock, mid, fc, payload)
            time.sleep(0.2)
        g.pump(3.0, until=lambda: g.truth is not None)

        def detumble():
            cfs_command(sock, ADCS_MID, ADCS_SET_MODE, bytes([BDOT_MODE]))
            return time.monotonic()

        def sunpoint():
            cfs_command(sock, ADCS_MID, ADCS_SET_MODE, bytes([SUNSAFE_MODE]))
            return time.monotonic()
        return measure(g, detumble, sunpoint, print)
    finally:
        cfs.terminate()


def compare(d):
    runs = {}
    for fsw in ("flatsat", "cfs"):
        f = pathlib.Path(d) / fsw / f"reference_{fsw}.json"
        runs[fsw] = json.loads(f.read_text()) if f.exists() else {}
    a, b = runs["flatsat"], runs["cfs"]

    def fmt(m, k, unit):
        return f"{m[k]} {unit}" if m.get(k) is not None else "not reached"
    ok1 = "detumble_s" in a and "detumble_s" in b
    print(f"{'PASS' if ok1 else 'FAIL'} [TC-15.1] detumble benchmark (informative) — time from 5.4 deg/s to below "
          f"{HANDOVER_DPS} deg/s: FlatSat {fmt(a, 'detumble_s', 's')}, NASA cFS ADCS {fmt(b, 'detumble_s', 's')}")
    ok2 = a.get("steady_error_deg") is not None and b.get("steady_error_deg") is not None
    print(f"{'PASS' if ok2 else 'FAIL'} [TC-15.2] sun pointing benchmark (informative) — to within 5 deg: FlatSat "
          f"{fmt(a, 'point_5deg_s', 's')}, NASA cFS ADCS {fmt(b, 'point_5deg_s', 's')}; steady-state error: FlatSat "
          f"{fmt(a, 'steady_error_deg', 'deg')}, NASA cFS ADCS {fmt(b, 'steady_error_deg', 'deg')}")
    return 0 if ok1 and ok2 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fsw", choices=["flatsat", "cfs"])
    ap.add_argument("--compare")
    a = ap.parse_args()
    if a.compare:
        return compare(a.compare)
    g = AdcsGround()
    m = run_flatsat(g) if a.fsw == "flatsat" else run_cfs(g)
    out = pathlib.Path(os.environ.get("SIL_LOG_DIR", ".")) / f"reference_{a.fsw}.json"
    out.write_text(json.dumps(m, indent=2))
    with open(out.with_suffix(".csv"), "w") as f:
        f.write("t_s,phase,rate_dps,sun_deg\n")
        for h in getattr(g, "history", []):
            f.write(f"{h['t_s']},{h['phase']},{h['rate_dps']},{h['sun_deg']}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
