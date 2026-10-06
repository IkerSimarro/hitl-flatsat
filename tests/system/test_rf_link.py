#!/usr/bin/env python3
"""System test: the RF link, end to end through the ground station software (ICD 7).

The test runs the ground station's contact model itself (GS_SET_CONTACT_MODE), and watches both paths: what
COSMOS would get over the radio (FLATSAT_RF, UDP 9031) and the OBC's own view over the umbilical (UDP 9011).
Runs in the SIL environment with the ground station out of contact at start (about 4 minutes):

    SIL_GS_ARGS="--mode never --seed 1" sil/sil.sh 'sil/start_nodes.sh &&
        (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) && python3 tests/system/test_rf_link.py'
"""
import pathlib
import select
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_obc_umbilical import Ground  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

RF_TM_PORT = 9031
RF_TC_ADDR = ("127.0.0.1", 9030)

results = []


def check(ok, name, detail):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name:46s} {detail}", flush=True)


class RfGround(Ground):
    """The umbilical ground (OBC's view) plus the radio path through the ground station (COSMOS's view)."""

    def __init__(self):
        super().__init__()
        self.rf_rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rf_rx.bind(("0.0.0.0", RF_TM_PORT))
        self.rf_latest, self.rf_counts, self.rf_events, self.rf_log = {}, {}, [], []
        self.rf_time = {}  # packet name -> header time (s): simulation time, for GS_STATUS
        self.rf_seq = 0

    def pump(self, seconds, until=None):
        deadline = time.monotonic() + seconds
        while until is None or not until():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            ready, _, _ = select.select([self.rx, self.rf_rx], [], [], remaining)
            if self.rf_rx in ready:
                data = self.rf_rx.recv(4096)
                try:
                    name, hdr, fields = icd.parse_telemetry(data)
                except ValueError as e:
                    print(f"  undecodable RF packet: {e}")
                    continue
                self.rf_latest[name] = fields
                self.rf_time[name] = hdr["seconds"]
                self.rf_counts[name] = self.rf_counts.get(name, 0) + 1
                self.rf_log.append((time.monotonic(), name, fields))
                if name == "EVENT":
                    text = fields["TEXT"].rstrip(b"\0").decode(errors="replace")
                    self.rf_events.append(text)
                    print(f"  RF EVENT: {text}", flush=True)
            if self.rx in ready:
                super().pump(0.001)

    def rf_send(self, cmd, **args):
        self.rf_seq += 1
        self.tx.sendto(icd.build_command(cmd, seq=self.rf_seq, **args), RF_TC_ADDR)

    def rf_wait(self, name, timeout, predicate=None):
        def found():
            return name in self.rf_latest and (predicate is None or predicate(self.rf_latest[name]))
        self.pump(timeout, until=found)
        return self.rf_latest.get(name) if found() else None

    def gs(self):
        return self.rf_latest.get("GS_STATUS", {})

    def stats(self):
        return self.latest["COMMS_STATS"][1]


def contact_mode(g, mode, on=0, off=0):
    g.rf_send("GS_SET_CONTACT_MODE", MODE=icd.CONTACT_MODE[mode], ON_TIME=on, OFF_TIME=off)
    return g.rf_wait("GS_STATUS", 5.0, lambda f: f["CONTACT_MODE"] == icd.CONTACT_MODE[mode])


def main():
    g = RfGround()
    g.wait_for("OBC_HK", 20.0)
    g.send("OBC_SET_AUTO_MODES", STATE=icd.SWITCH_STATE["OFF"])  # a quiet spacecraft: this test is about the link

    # ---- 1. Ground station up, out of contact, predicting the next pass ----
    gs = g.rf_wait("GS_STATUS", 10.0, lambda f: f["RANGE"] > 0)
    check(gs is not None and gs["CONTACT"] == 0 and gs["CONTACT_MODE"] == icd.CONTACT_MODE["NEVER"] and
          gs["NEXT_AOS"] != 0xFFFFFFFF and 300 < gs["RANGE"] < 13000,
          "[TC-09.1] ground station status, next pass predicted",
          f"elevation {gs['ELEVATION']:.1f} deg, range {gs['RANGE']:.0f} km, next pass over the ground station in "
          f"{gs['NEXT_AOS'] / 60:.1f} min ({gs['NEXT_PASS_DURATION']} s, max {gs['NEXT_MAX_ELEVATION'] / 10:.1f} deg)"
          if gs else "no GS_STATUS")

    # ---- 2. No contact: the spacecraft beacons anyway; nothing reaches COSMOS; data waits on board ----
    g.pump(3.0)
    tx0, no_contact0 = g.stats()["RF_TX_FRAMES"], g.gs()["DOWN_NO_CONTACT"]
    g.send("OBC_NOOP")                              # its event goes into the RF queue
    g.rf_send("COMMS_NOOP")                         # waits in the ground station's uplink queue
    g.pump(22.0)
    st, gs = g.stats(), g.gs()
    check(st["CONTACT"] == 0 and st["RF_TX_FRAMES"] >= tx0 + 2 and gs["DOWN_NO_CONTACT"] >= no_contact0 + 2 and
          "BEACON" not in g.rf_counts and st["TX_QUEUE_DEPTH"] >= 1 and gs["UP_QUEUE"] == 1,
          "[TC-09.2] out of contact: beacons unheard, data held",
          f"OBC sent {st['RF_TX_FRAMES'] - tx0} beacons, ground saw {gs['DOWN_NO_CONTACT'] - no_contact0} outside "
          f"contact; {st['TX_QUEUE_DEPTH']} packets waiting on board, {gs['UP_QUEUE']} telecommand at the ground, "
          f"airtime {st['DUTY_CYCLE'] / 10:.1f} %")

    # ---- 3. AOS: hail, the queued telecommand goes up, the stored events come down ----
    t_aos = time.monotonic()
    contact_mode(g, "ALWAYS")
    hk = g.wait_for("COMMS_STATS", 10.0, lambda f: f["CONTACT"] == 1)
    g.pump(8.0, until=lambda: any("COMMS NOOP received via RF" in e for e in g.rf_events) and
           any("NOOP received" in e and "COMMS" not in e for e in g.rf_events))
    check(hk is not None and any("COMMS NOOP received via RF" in e for e in g.rf_events) and
          any(e.startswith("NOOP received") for e in g.rf_events) and g.gs()["UP_QUEUE"] == 0,
          "[TC-09.3] AOS: stored data down, queued command up",
          f"OBC in contact {time.monotonic() - t_aos:.1f} s after AOS; on-board events delivered: "
          f"{len(g.rf_events)}; the COMMS_NOOP queued before AOS was executed")
    b0 = g.rf_counts.get("BEACON", 0)
    g.pump(21.0)
    check(g.rf_counts.get("BEACON", 0) - b0 >= 2, "[TC-09.4] beacons heard in contact",
          f"{g.rf_counts.get('BEACON', 0) - b0} in 21 s, last: mode {g.rf_latest['BEACON']['MODE']}, "
          f"uptime {g.rf_latest['BEACON']['UPTIME']} s, RSSI {g.gs()['LAST_RSSI']} dBm, "
          f"SNR {g.gs()['LAST_SNR'] / 4:.1f} dB")

    # ---- 4. Round trip over RF, and a packet downlinked on request ----
    t0 = time.monotonic()
    g.rf_send("OBC_PING", TOKEN=0xC0FFEE)
    rep = g.rf_wait("PING_REPLY", 10.0, lambda f: f["TOKEN"] == 0xC0FFEE)
    rtt = time.monotonic() - t0
    g.rf_send("OBC_DOWNLINK_PACKET", TLM_MID=icd.TLM["EPS_SIM"][0])
    eps = g.rf_wait("EPS_SIM", 10.0)
    check(rep is not None and eps is not None, "[TC-09.5] ping and packet on request over RF",
          f"PING_REPLY after {rtt:.2f} s round trip; EPS_SIM on request: battery {eps['BATT_V']:.2f} V"
          if rep and eps else f"ping {rep is not None}, EPS_SIM {eps is not None}")

    # ---- 5. Corrupted uplink frames are rejected and counted ----
    crc0 = g.stats()["RF_CRC_ERRORS"]
    g.rf_send("GS_CORRUPT_NEXT", COUNT=2)
    g.pump(1.0)
    g.rf_send("COMMS_NOOP")
    g.rf_send("COMMS_NOOP")
    st = g.wait_for("COMMS_STATS", 10.0, lambda f: f["RF_CRC_ERRORS"] >= crc0 + 2)
    check(st is not None, "[TC-09.6] corrupted uplink frames rejected (CRC)",
          f"OBC RF_CRC_ERRORS {crc0} -> {g.stats()['RF_CRC_ERRORS']}")

    # ---- 6. Frame loss on the link ----
    lost0, down0 = g.gs()["DOWN_LOST"], g.gs()["DOWN_FRAMES"]
    g.rf_send("GS_SET_LOSS", LOSS=500)
    g.pump(60.0)
    g.rf_send("GS_SET_LOSS", LOSS=0)
    gs = g.rf_wait("GS_STATUS", 5.0, lambda f: f["LOSS_RATE"] == 0)
    lost, down = g.gs()["DOWN_LOST"] - lost0, g.gs()["DOWN_FRAMES"] - down0
    check(lost >= 1 and down >= 1, "[TC-09.7] 50 % frame loss: some frames lost, some through",
          f"{lost} lost, {down} delivered in 60 s")

    # ---- 7. LOS: the spacecraft notices the silence ----
    n = len(g.events)
    contact_mode(g, "NEVER")
    t_los = time.monotonic()
    st = g.wait_for("COMMS_STATS", 60.0, lambda f: f["CONTACT"] == 0)
    check(st is not None and any("RF contact lost" in e[2] for e in g.events[n:]), "[TC-09.8] LOS: contact lost on board",
          f"after {time.monotonic() - t_los:.0f} s without hails (45 s timeout)")

    # ---- 8. Beacon period: off, invalid, back on ----
    rejects = g.latest["OBC_HK"][1]["CMD_REJECT_COUNT"]
    g.send("COMMS_SET_BEACON_PERIOD", PERIOD=0)
    g.pump(2.0)
    tx0 = g.stats()["RF_TX_FRAMES"]
    g.pump(15.0)
    tx_off = g.stats()["RF_TX_FRAMES"] - tx0
    g.send("COMMS_SET_BEACON_PERIOD", PERIOD=3)
    g.send("COMMS_SET_BEACON_PERIOD", PERIOD=10)
    st = g.wait_for("COMMS_STATS", 5.0, lambda f: f["BEACON_PERIOD"] == 10)
    check(tx_off == 0 and st is not None and g.latest["OBC_HK"][1]["CMD_REJECT_COUNT"] == rejects + 1,
          "[TC-09.9] beacon off, 3 s rejected, back to 10 s", f"{tx_off} frames sent in 15 s with the beacon off")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
