#!/usr/bin/env python3
"""System test: the OBC with the ADCS and EPS nodes on the CAN bus, operated over the umbilical.

Runs in the SIL environment with the plant model and the nodes:
    sil/sil.sh 'sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&
                python3 tests/system/test_can_nodes.py'
"""
import math
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_obc_umbilical import Ground  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

RPM = 2 * math.pi / 60  # rad/s per rpm
ALL_NODES = 0x0E        # OBC, ADCS, EPS
DRIVER_ENABLED = 0x01
AT_SETPOINT = 0x02

results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:38s} {detail}")


def event_seen(g, text, since):
    return any(text in e[2] for e in g.events[since:])


def fresh(g, name, timeout=3.0, predicate=None):
    """The next packet received (wait_for alone can return a cached one)."""
    g.latest.pop(name, None)
    return g.wait_for(name, timeout, predicate)


def main():
    g = Ground()
    g.wait_for("OBC_HK", 20.0)
    # NOS3's 2.8 deg/s deployment tip-off would start automatic detumbling; this test needs a quiet spacecraft
    # (tests/system/test_adcs.py covers the automatic modes)
    g.send("OBC_SET_AUTO_MODES", STATE=icd.SWITCH_STATE["OFF"])

    # ---- Nodes up ----
    # (the "node up" events go out before the umbilical link is, so the alive mask is the evidence here)
    hk = g.wait_for("OBC_HK", 20.0, lambda f: f["NODE_ALIVE_MASK"] == ALL_NODES)
    rx0 = hk["CAN_RX_COUNT"] if hk else 0
    hk2 = g.wait_for("OBC_HK", 3.0, lambda f: f["CAN_RX_COUNT"] > rx0)
    check(hk is not None and hk2 is not None and hk2["CAN_ERROR_COUNT"] == 0, "ADCS and EPS nodes up",
          f"alive mask 0x{g.latest['OBC_HK'][1]['NODE_ALIVE_MASK']:02X}, CAN rx {rx0} -> "
          f"{g.latest['OBC_HK'][1]['CAN_RX_COUNT']}, errors {g.latest['OBC_HK'][1]['CAN_ERROR_COUNT']}")

    # ---- Real power ----
    eps = g.wait_for("EPS_REAL", 5.0, lambda f: f["BATT_MV"] > 0)
    if eps:
        ma = [eps[f"RAIL_MA_{i}"] for i in range(3)]
        check(3600 < eps["BATT_MV"] < 4200 and 40 <= ma[1] <= 80 and 30 <= ma[2] <= 60 and
              ma[0] >= ma[1] + ma[2] and eps["SWITCH_MASK"] == 0x03,
              "EPS_REAL power telemetry", f"battery {eps['BATT_MV']} mV, rails {ma} mA, switches "
              f"0x{eps['SWITCH_MASK']:02X}, charge {eps['CHARGE_STATE']}")
        idle_adcs_ma = ma[2]
    else:
        check(False, "EPS_REAL power telemetry", "no EPS_REAL")
        idle_adcs_ma = 0

    # ---- Wheel speed step in TEST mode ----
    g.send("OBC_SET_MODE", MODE=icd.MODE["TEST"])
    g.send("ADCS_PHYS_WHEEL_TEST", CTRL_MODE=icd.RW_CTRL_MODE["SPEED"], SETPOINT=300)
    t0 = time.monotonic()
    st = g.wait_for("ADCS_STATE", 5.0, lambda f: abs(f["PHYS_RW_MEAS_SPEED"] - 300 * RPM) < 20 * RPM and
                    f["PHYS_RW_STATUS"] & AT_SETPOINT)
    t_settle = time.monotonic() - t0
    check(st is not None, "physical wheel tracks 300 rpm",
          f"{st['PHYS_RW_MEAS_SPEED'] / RPM:.0f} rpm after {t_settle:.1f} s (incl. telemetry latency)"
          if st else "never reached 300 +- 20 rpm")
    # EPS samples at 10 Hz but reports at 1 Hz, and EPS_REAL is 1 Hz: up to 2 s behind
    eps = fresh(g, "EPS_REAL", 4.0, lambda f: f["RAIL_MA_2"] > idle_adcs_ma + 10)
    check(eps is not None, "motor current visible on the ADCS rail",
          f"{g.latest['EPS_REAL'][1]['RAIL_MA_2']} mA vs {idle_adcs_ma} mA idle")

    # ---- Motor driver cut (fault injection through the EPS switch) ----
    n = len(g.events)
    g.send("EPS_SWITCH", SWITCH_ID=1, STATE=icd.SWITCH_STATE["OFF"])
    st = g.wait_for("ADCS_STATE", 3.0, lambda f: not f["PHYS_RW_STATUS"] & DRIVER_ENABLED)
    g.pump(3.0)
    coast = g.latest["ADCS_STATE"][1]["PHYS_RW_MEAS_SPEED"] / RPM
    check(st is not None and event_seen(g, "ADCS node fault code 2", n) and coast < 150,
          "motor switch OFF: fault reported, wheel coasts", f"driver-enabled bit cleared, fault event "
          f"{event_seen(g, 'fault code 2', n)}, {coast:.0f} rpm after 3 s")
    g.send("EPS_SWITCH", SWITCH_ID=1, STATE=icd.SWITCH_STATE["ON"])
    st = g.wait_for("ADCS_STATE", 5.0, lambda f: abs(f["PHYS_RW_MEAS_SPEED"] - 300 * RPM) < 20 * RPM)
    check(st is not None, "motor switch ON: wheel back to 300 rpm",
          f"{st['PHYS_RW_MEAS_SPEED'] / RPM:.0f} rpm" if st else "did not recover")

    # ---- ADCS node power cycle (RUN pin held low by the EPS) ----
    n = len(g.events)
    g.send("EPS_SWITCH", SWITCH_ID=0, STATE=icd.SWITCH_STATE["OFF"])
    lost = g.wait_for("OBC_HK", 6.0, lambda f: f["NODE_ALIVE_MASK"] == ALL_NODES & ~0x04)
    eps = fresh(g, "EPS_REAL")
    check(lost is not None and event_seen(g, "ADCS node lost", n) and eps is not None and eps["RAIL_MA_2"] < 20,
          "ADCS held in reset: node lost",
          f"alive mask 0x{g.latest['OBC_HK'][1]['NODE_ALIVE_MASK']:02X}, ADCS rail {eps['RAIL_MA_2']} mA")
    n = len(g.events)
    g.send("EPS_SWITCH", SWITCH_ID=0, STATE=icd.SWITCH_STATE["ON"])
    back = g.wait_for("OBC_HK", 6.0, lambda f: f["NODE_ALIVE_MASK"] == ALL_NODES)
    st = g.wait_for("ADCS_STATE", 6.0, lambda f: abs(f["PHYS_RW_MEAS_SPEED"] - 300 * RPM) < 20 * RPM)
    check(back is not None and event_seen(g, "reset cause 0", n) and st is not None,
          "ADCS released: power-on reboot, resumes", "node up with reset cause POWER_ON, wheel back at 300 rpm"
          if back and st else f"mask 0x{g.latest['OBC_HK'][1]['NODE_ALIVE_MASK']:02X}, wheel {st is not None}")

    # ---- Commanded node reset ----
    n = len(g.events)
    g.send("OBC_NODE_RESET", NODE=icd.NODE["ADCS"])
    g.pump(4.0)
    check(event_seen(g, "acknowledged command 2", n) and event_seen(g, "rebooted (reset cause 2)", n),
          "OBC_NODE_RESET ADCS", "acknowledged, node rebooted with reset cause COMMAND")

    # ---- Back to mirroring when leaving TEST ----
    g.send("OBC_SET_MODE", MODE=icd.MODE["SAFE"])
    st = g.wait_for("ADCS_STATE", 3.0, lambda f: f["PHYS_RW_CMD_SPEED"] == 0)
    check(st is not None, "leaving TEST ends the override", "physical wheel commanded off in SAFE")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
