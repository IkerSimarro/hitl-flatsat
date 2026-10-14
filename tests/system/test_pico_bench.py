#!/usr/bin/env python3
"""System test TC-16: the OBC on the Raspberry Pi Pico 2, alone on the bench (hardware in the loop).

The flight software runs on the Pico; its umbilical is the Pico's USB port, attached to WSL as /dev/ttyACM0
(usbipd) and opened by the HIL bridge. The CAN boards aren't fitted, so the OBC runs without its nodes.
  1. commanded restart: the Pico reboots, its USB port disappears and comes back, and the bridge reopens it;
     the OBC reports the restart's cause. Every run starts from the same state this way
  2. bench configuration: no CAN bus, so no nodes alive and none waited for
  3. from NOS3's deployment tip-off: tumble detected, detumble, sun pointing, all on its own
  4. sun pointing converges, checked against 42's truth
  5. health: a minute of sun pointing with no event of WARNING severity or above

    tools/pico/flash.sh && SIL_UMB_DEV=/dev/ttyACM0 sil/sil.sh 'python3 tests/system/test_pico_bench.py'
"""
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_adcs import AdcsGround, wait_mode  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

REBOOT_MAGIC = 0x0B0075ED
results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:44s}  {detail}", flush=True)


def main():
    g = AdcsGround()
    if g.wait_for("OBC_HK", 30.0) is None:
        check(False, "[TC-16.1] commanded restart over USB", "no telemetry from the Pico in 30 s")
        return 1

    # ---- 1. Commanded restart ----
    g.send("OBC_REBOOT", MAGIC=REBOOT_MAGIC)
    t0 = time.monotonic()
    g.latest.pop("OBC_HK", None)
    g.pump(1.0)  # telemetry stops while the Pico restarts
    hk = g.wait_for("OBC_HK", 20.0, lambda f: f["UPTIME"] < 10)
    t_back = time.monotonic() - t0
    check(hk is not None and hk["RESET_CAUSE"] == icd.RESET_CAUSE["COMMAND"], "[TC-16.1] commanded restart over USB",
          f"telemetry back {t_back:.1f} s after OBC_REBOOT, uptime {hk['UPTIME']} s, reset cause "
          f"{hk['RESET_CAUSE']} (COMMAND)" if hk else "no telemetry after the restart")
    if hk is None:
        return 1
    n_events = len(g.events)

    # ---- 2. Bench configuration ----
    check(hk["NODE_ALIVE_MASK"] == 1 << icd.NODE["OBC"], "[TC-16.2] CAN bus not fitted",
          f"node alive mask 0x{hk['NODE_ALIVE_MASK']:02X} (the OBC only)")

    # ---- 3. Detumble and sun pointing ----
    g.pump(3.0, until=lambda: g.truth is not None)
    rate0 = g.truth_rate_deg() if g.truth else float("nan")
    t_det = wait_mode(g, "DETUMBLE", 60.0)
    t_det = time.monotonic() - t0 if t_det is not None else None
    t_sun = wait_mode(g, "SUN_POINT", 400.0)
    t_sun = time.monotonic() - t0 if t_sun is not None else None
    check(t_det is not None and t_sun is not None, "[TC-16.3] detumble and hand-over",
          f"from {rate0:.1f} deg/s (42 truth): DETUMBLE {t_det:.0f} s, SUN_POINT {t_sun:.0f} s after the restart, "
          f"{g.truth_rate_deg():.2f} deg/s" if t_sun is not None else f"DETUMBLE {t_det}, SUN_POINT not reached")
    if t_sun is None:
        return 1

    # ---- 4. Convergence against 42's truth ----
    st = g.wait_for("ADCS_STATE", 300.0, lambda f: f["CONVERGED"] == 1)
    err = g.truth_sun_error_deg()
    check(st is not None and err < 5.0, "[TC-16.4] sun pointing converges",
          f"converged {time.monotonic() - t0:.0f} s after the restart; Sun angle {err:.2f} deg (42 truth), "
          f"OBC estimate {st['POINTING_ERROR'] / icd_deg():.2f} deg" if st else "not converged in 300 s")

    # ---- 5. Health ----
    worst, end = 0.0, time.monotonic() + 60.0
    while time.monotonic() < end:
        g.pump(1.0)
        worst = max(worst, g.truth_sun_error_deg())
    bad = [e[2] for e in g.events[n_events:] if e[1] >= icd.SEVERITY["WARNING"]]
    mode = g.latest["OBC_HK"][1]["MODE"]
    check(not bad and mode == icd.MODE["SUN_POINT"] and worst < 2.0, "[TC-16.5] a minute of sun pointing",
          f"mode {mode} (SUN_POINT), worst Sun angle {worst:.2f} deg (42 truth), {len(bad)} events of WARNING "
          f"severity or above" + (f": {bad[:3]}" if bad else ""))

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


def icd_deg():
    return 3.141592653589793 / 180.0


if __name__ == "__main__":
    sys.exit(main())
