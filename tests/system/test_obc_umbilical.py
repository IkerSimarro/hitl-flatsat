#!/usr/bin/env python3
"""System test: OBC commanding and telemetry over the umbilical, in the software-in-the-loop environment.

Acts as COSMOS would: receives the bridge's telemetry on UDP 9011 and sends commands to UDP 9010.

    sil/sil.sh 'sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&
                python3 tests/system/test_obc_umbilical.py'
"""
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_icd as icd  # noqa: E402

TM_PORT = 9011
TC_ADDR = ("127.0.0.1", 9010)


class Ground:
    def __init__(self):
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("0.0.0.0", TM_PORT))
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.latest = {}
        self.events = []
        self.counts = {}
        self.seq = 0

    def pump(self, seconds, until=None):
        """Receive telemetry for a while, keeping the latest packet of each kind; stop early when until() is true."""
        deadline = time.monotonic() + seconds
        while until is None or not until():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            self.rx.settimeout(remaining)
            try:
                data = self.rx.recv(4096)
            except socket.timeout:
                return
            try:
                name, hdr, fields = icd.parse_telemetry(data)
            except ValueError as e:
                print(f"  undecodable packet: {e}")
                continue
            self.latest[name] = (hdr, fields)
            self.counts[name] = self.counts.get(name, 0) + 1
            if name == "EVENT":
                text = fields["TEXT"].rstrip(b"\0").decode(errors="replace")
                self.events.append((fields["EVENT_ID"], fields["SEVERITY"], text))
                print(f"  EVENT {fields['EVENT_ID']}: {text}")

    def wait_for(self, name, timeout=5.0, predicate=None):
        def found():
            return name in self.latest and (predicate is None or predicate(self.latest[name][1]))
        self.pump(timeout, until=found)
        return self.latest[name][1] if found() else None

    def send(self, cmd, raw=None, **args):
        self.seq += 1
        pkt = raw if raw is not None else icd.build_command(cmd, seq=self.seq, **args)
        self.tx.sendto(pkt, TC_ADDR)

    def hk(self):
        """Fresh OBC_HK (waits for the next one)."""
        self.latest.pop("OBC_HK", None)
        return self.wait_for("OBC_HK", 3.0)


results = []


def check(ok, name, detail):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {name:34s} {detail}")


def main():
    g = Ground()
    g.wait_for("OBC_HK", 20.0)
    # NOS3's 2.8 deg/s deployment tip-off would start automatic detumbling; this test needs a quiet spacecraft
    # (tests/system/test_adcs.py covers the automatic modes)
    g.send("OBC_SET_AUTO_MODES", STATE=icd.SWITCH_STATE["OFF"])

    # Telemetry flowing at the default rates
    g.pump(1.0)
    g.counts.clear()
    g.pump(5.0)
    hk_rate = g.counts.get("OBC_HK", 0) / 5.0
    check(g.wait_for("OBC_HK", 20.0) is not None and 0.6 <= hk_rate <= 1.4, "OBC_HK at 1 Hz",
          f"{hk_rate:.1f} Hz, packets seen: {dict(sorted(g.counts.items()))}")

    hdr, hk = g.latest["OBC_HK"]
    check(hk["MODE"] == icd.MODE["SAFE"] and hk["TIME_SOURCE"] == icd.TIME_SOURCE["UMBILICAL"],
          "boot state", f"mode {hk['MODE']} (SAFE), time source {hk['TIME_SOURCE']} (UMBILICAL), "
          f"packet time J2000 {hdr['seconds']} s")

    # Sensors all valid within a couple of acquisition cycles (GPS starts 10 s into the simulation)
    sens = g.wait_for("ADCS_SENSORS", 15.0, lambda f: f["VALID_MASK"] == 0x3F)
    mask = g.latest.get("ADCS_SENSORS", ({}, {"VALID_MASK": 0}))[1]["VALID_MASK"]
    check(sens is not None, "all sensors valid", f"valid mask 0x{mask:02X} (expected 0x3F)")
    if sens:
        rate = [sens[f"IMU_RATE_{i}"] for i in range(3)]
        mag = [sens[f"MAG_{i}"] * 1e6 for i in range(3)]
        print(f"     body rate {[round(r * 57.2958, 3) for r in rate]} deg/s, field {[round(m, 2) for m in mag]} uT")

    eps = g.wait_for("EPS_SIM", 3.0)
    check(eps is not None and 10 < eps["BATT_V"] < 40, "EPS_SIM telemetry",
          f"battery {eps['BATT_V']:.2f} V, solar array {eps['SA_V']:.2f} V" if eps else "missing")

    # NOOP: accepted, counted, event raised
    before = g.hk()["CMD_ACCEPT_COUNT"]
    g.send("OBC_NOOP")
    g.pump(1.5)
    after = g.hk()
    check(after["CMD_ACCEPT_COUNT"] == before + 1 and after["LAST_CMD_FC"] == 0 and
          any("NOOP" in e[2] for e in g.events), "OBC_NOOP", f"accept count {before} -> {after['CMD_ACCEPT_COUNT']}")

    # Corrupted checksum: rejected with an event
    before = after["CMD_REJECT_COUNT"]
    bad = bytearray(icd.build_command("OBC_NOOP"))
    bad[7] ^= 0x55
    g.send(None, raw=bytes(bad))
    g.pump(1.5)
    after = g.hk()
    check(after["CMD_REJECT_COUNT"] == before + 1 and any("bad checksum" in e[2] for e in g.events),
          "bad checksum rejected", f"reject count {before} -> {after['CMD_REJECT_COUNT']}")

    # Wrong length for a known command: rejected
    before = after["CMD_REJECT_COUNT"]
    short = bytearray(icd.build_command("OBC_PING", TOKEN=1)[:-1])
    short[5] -= 1  # keep the CCSDS length field consistent so only the ICD length check catches it
    short[7] = 0
    x = 0
    for b in short:
        x ^= b
    short[7] = x ^ 0xFF
    g.send(None, raw=bytes(short))
    g.pump(1.5)
    after = g.hk()
    check(after["CMD_REJECT_COUNT"] == before + 1, "wrong-length command rejected",
          f"reject count {before} -> {after['CMD_REJECT_COUNT']}")

    # Mode changes: SAFE -> TEST allowed; TEST -> DETUMBLE refused; TEST -> SAFE allowed
    g.send("OBC_SET_MODE", MODE=icd.MODE["TEST"])
    g.pump(1.5)
    m1 = g.hk()["MODE"]
    g.send("OBC_SET_MODE", MODE=icd.MODE["DETUMBLE"])
    g.pump(1.5)
    m2 = g.hk()["MODE"]
    g.send("OBC_SET_MODE", MODE=icd.MODE["SAFE"])
    g.pump(1.5)
    m3 = g.hk()
    check(m1 == icd.MODE["TEST"] and m2 == icd.MODE["TEST"] and m3["MODE"] == icd.MODE["SAFE"] and
          m3["MODE_REASON"] == icd.MODE_REASON["COMMAND"], "mode transitions",
          f"SAFE->TEST {m1}, TEST->DETUMBLE refused (still {m2}), TEST->SAFE {m3['MODE']}")

    # Ping: each reply carries its token; round trip ground -> bridge -> OBC -> bridge -> ground
    rtts = []
    for n in range(20):
        token = 0xC0FFEE00 + n
        t0 = time.monotonic()
        g.send("OBC_PING", TOKEN=token)
        rep = g.wait_for("PING_REPLY", 2.0, lambda f, t=token: f["TOKEN"] == t)
        if rep is not None:
            rtts.append((time.monotonic() - t0) * 1000)
        time.sleep(0.037)  # spread requests across the OBC's 50 ms command cycle
    rtts.sort()
    detail = (f"{len(rtts)}/20 replies, round trip min {rtts[0]:.1f} / median {rtts[len(rtts) // 2]:.1f} / "
              f"max {rtts[-1]:.1f} ms" if rtts else "no replies")
    # Commands are executed by a 50 ms task, so up to ~50 ms plus transport is expected
    check(len(rtts) == 20 and rtts[-1] < 100, "OBC_PING x20", detail)

    # Telemetry rate change: EPS_SIM off, then back to 1 s
    g.send("OBC_SET_TLM_PERIOD", TLM_MID=icd.TLM["EPS_SIM"][0], PERIOD=0)
    g.pump(1.0)
    g.counts.clear()
    g.pump(3.0)
    off = g.counts.get("EPS_SIM", 0)
    g.send("OBC_SET_TLM_PERIOD", TLM_MID=icd.TLM["EPS_SIM"][0], PERIOD=1000)
    g.pump(1.0)
    g.counts.clear()
    g.pump(3.0)
    on = g.counts.get("EPS_SIM", 0)
    check(off == 0 and on >= 2, "OBC_SET_TLM_PERIOD", f"EPS_SIM packets in 3 s: off {off}, back on {on}")

    # Simulated EPS switch round trip, confirmed in EPS_SIM
    g.send("EPS_SIM_SWITCH", SWITCH_ID=7, STATE=icd.SWITCH_STATE["ON"])
    on = g.wait_for("EPS_SIM", 4.0, lambda f: f["SWITCH_MASK"] & 0x80)
    g.send("EPS_SIM_SWITCH", SWITCH_ID=7, STATE=icd.SWITCH_STATE["OFF"])
    off = g.wait_for("EPS_SIM", 4.0, lambda f: not f["SWITCH_MASK"] & 0x80)
    check(on is not None and off is not None, "EPS_SIM_SWITCH", "switch 7 on then off, seen in EPS_SIM")

    # Manual torquer command refused outside TEST mode
    before = g.hk()["CMD_REJECT_COUNT"]
    g.send("ADCS_TRQ_MANUAL", TORQUER=0, DUTY=5000)
    g.pump(1.5)
    check(g.hk()["CMD_REJECT_COUNT"] == before + 1, "TRQ_MANUAL refused in SAFE", "rejected with an event")

    passed = sum(results)
    print(f"---- {passed} passed, {len(results) - passed} failed ----")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
