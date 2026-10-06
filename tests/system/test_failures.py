#!/usr/bin/env python3
"""System test TC-12: actuator, node and bus failures (fault injection beyond the sensors).

In the order the scenario reaches the modes they matter in:
  1. magnetometer failure during the detumble (DETUMBLE needs it)       -> SAFE, then automatic restart
  2. reaction wheel failure while sun pointing (SUN_POINT needs it)      -> SAFE
  3. ADCS node crash while sun pointing                                  -> node lost, SAFE, restart reported
  4. EPS node crash                                                      -> node lost, restart reported
  5. CAN bus loss with the physical wheel running                        -> both nodes lost; the ADCS node stops
                                                                            the wheel on its own; recovery
Runs in the SIL environment from NOS3's 2.8 deg/s deployment tip-off (about 8 minutes):

    sil/sil.sh 'sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&
                python3 tests/system/test_failures.py'
"""
import mmap
import os
import pathlib
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_obc_umbilical import Ground  # noqa: E402
from test_adcs import sim_command, event_seen, wait_mode  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "ground"))
import flatsat_icd as icd  # noqa: E402

LOG_DIR = pathlib.Path(os.environ.get("SIL_LOG_DIR", "/tmp"))
NODES = {"adcs": ["--can", "vcan0", "--run-line", "adcs"], "eps": ["--can", "vcan0"]}
ALL_NODES = 0x0E
RPM = 2 * 3.141592653589793 / 60

results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:50s} {detail}", flush=True)


def motor_duty():
    """The PWM duty the ADCS node is driving right now, from the plant harness (sil_harness.h: offset 8)."""
    with open("/dev/shm/flatsat-harness", "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            return struct.unpack_from("f", m, 8)[0]
        finally:
            m.close()


def kill_node(node):
    subprocess.run(["pkill", "-9", "-f", f"firmware/build/{node}_node"], check=False)


def start_node(node):
    log = open(LOG_DIR / f"{node}_node.log", "a")
    subprocess.Popen([str(ROOT / f"firmware/build/{node}_node")] + NODES[node], stdout=log, stderr=subprocess.STDOUT,
                     start_new_session=True)


def vcan(*args):
    subprocess.run([str(ROOT / "firmware/build/vcan_up"), "vcan0", *args], check=True, stdout=subprocess.DEVNULL)


def wait_event(g, text, since, timeout):
    t0 = time.monotonic()
    g.pump(timeout, until=lambda: event_seen(g, text, since))
    return time.monotonic() - t0 if event_seen(g, text, since) else None


def main():
    g = Ground()
    g.wait_for("OBC_HK", 20.0)

    # ---- 1. Magnetometer failure during the detumble ----
    if wait_mode(g, "DETUMBLE", 60.0) is None:
        check(False, "[TC-12.2] magnetometer failure in DETUMBLE", "never reached DETUMBLE")
        return 1
    g.pump(5.0)
    n = len(g.events)
    sim_command("mag-command", "DISABLE")
    t_safe = wait_event(g, "magnetometer failed in DETUMBLE: SAFE", n, 15.0)
    hk = g.wait_for("OBC_HK", 3.0, lambda f: f["MODE"] == icd.MODE["SAFE"])
    sim_command("mag-command", "ENABLE")
    t_back = wait_event(g, "magnetometer recovered", n, 10.0)
    restarted = wait_mode(g, "DETUMBLE", 30.0)
    check(t_safe is not None and t_safe < 10.0 and hk is not None and hk["MODE_REASON"] == icd.MODE_REASON["FAULT"]
          and t_back is not None and restarted is not None,
          "[TC-12.2] magnetometer failure in DETUMBLE",
          f"declared failed and SAFE {t_safe:.1f} s after the magnetometer stopped; recovered, and DETUMBLE again "
          f"on its own" if t_safe is not None else "no fault response")

    # ---- 2. Reaction wheel failure while sun pointing ----
    st = g.wait_for("ADCS_STATE", 600.0, lambda f: f["CONVERGED"] == 1)
    if st is None:
        check(False, "[TC-12.1] reaction wheel failure in SUN_POINT", "sun pointing never converged")
        return 1
    n = len(g.events)
    sim_command("rw1-command", "DISABLE")
    t_safe = wait_event(g, "reaction wheels failed in SUN_POINT: SAFE", n, 15.0)
    sim_command("rw1-command", "ENABLE")
    t_back = wait_event(g, "reaction wheels recovered", n, 10.0)
    check(t_safe is not None and t_safe < 10.0 and event_seen(g, "reaction wheels failed:", n) and t_back is not None,
          "[TC-12.1] reaction wheel failure in SUN_POINT",
          f"wheels declared failed and SAFE {t_safe:.1f} s after wheel 1 stopped answering; recovered"
          if t_safe is not None else "no fault response")

    # ---- 3. ADCS node crash while sun pointing ----
    g.send("OBC_SET_MODE", MODE=icd.MODE["SUN_POINT"])
    st = g.wait_for("ADCS_STATE", 240.0, lambda f: f["CONVERGED"] == 1)
    n = len(g.events)
    kill_node("adcs")
    t_lost = wait_event(g, "ADCS node lost", n, 10.0)
    hk = g.wait_for("OBC_HK", 3.0, lambda f: f["MODE"] == icd.MODE["SAFE"])
    start_node("adcs")
    t_up = wait_event(g, "ADCS node up", n, 10.0)
    check(st is not None and t_lost is not None and t_lost < 5.0 and hk is not None and t_up is not None,
          "[TC-12.3] ADCS node crash in SUN_POINT",
          f"node lost reported {t_lost:.1f} s after the crash, SAFE (reason {hk['MODE_REASON'] if hk else '?'}); "
          f"restart reported" if t_lost is not None else "node loss not reported")

    # ---- 4. EPS node crash ----
    n = len(g.events)
    kill_node("eps")
    t_lost = wait_event(g, "EPS node lost", n, 10.0)
    start_node("eps")
    t_up = wait_event(g, "EPS node up", n, 10.0)
    eps = g.wait_for("EPS_REAL", 5.0, lambda f: f["EPS_UPTIME"] < 10)
    check(t_lost is not None and t_lost < 5.0 and t_up is not None and eps is not None,
          "[TC-12.4] EPS node crash",
          f"node lost reported {t_lost:.1f} s after the crash; restarted node reporting power again"
          if t_lost is not None else "node loss not reported")

    # ---- 5. CAN bus loss with the physical wheel running ----
    g.send("OBC_SET_MODE", MODE=icd.MODE["TEST"])
    g.send("ADCS_PHYS_WHEEL_TEST", CTRL_MODE=icd.RW_CTRL_MODE["SPEED"], SETPOINT=300)
    spinning = g.wait_for("ADCS_STATE", 10.0, lambda f: abs(f["PHYS_RW_MEAS_SPEED"] - 300 * RPM) < 20 * RPM)
    n = len(g.events)
    log_lines = len((LOG_DIR / "adcs_node.log").read_text().splitlines())
    vcan("down")
    t0 = time.monotonic()
    t_drive_off = None
    while time.monotonic() - t0 < 3.0:
        if t_drive_off is None and abs(motor_duty()) < 1e-6:
            t_drive_off = time.monotonic() - t0
        g.pump(0.05)
    lost = g.wait_for("OBC_HK", 5.0, lambda f: f["NODE_ALIVE_MASK"] & ALL_NODES == 0x02)
    t_lost = time.monotonic() - t0
    both = event_seen(g, "ADCS node lost", n) and event_seen(g, "EPS node lost", n)
    g.pump(2.0)
    node_safe = "OBC heartbeat lost" in "\n".join((LOG_DIR / "adcs_node.log").read_text().splitlines()[log_lines:])
    vcan()
    back = g.wait_for("OBC_HK", 10.0, lambda f: f["NODE_ALIVE_MASK"] == ALL_NODES)
    g.send("OBC_SET_MODE", MODE=icd.MODE["SAFE"])
    check(spinning is not None and lost is not None and t_lost < 6.0 and both and t_drive_off is not None and
          t_drive_off < 1.5 and node_safe and back is not None,
          "[TC-12.5] CAN bus loss",
          f"both nodes lost after {t_lost:.1f} s; the ADCS node removed motor drive {t_drive_off:.2f} s after the bus "
          f"went down and went to its safe state; nodes back after the bus returned"
          if t_drive_off is not None and lost is not None else
          f"spinning {spinning is not None}, lost {lost is not None}, drive off {t_drive_off}, node safe {node_safe}, "
          f"back {back is not None}")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
