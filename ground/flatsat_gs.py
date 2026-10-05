#!/usr/bin/env python3
"""FlatSat ground station software (ICD 7.4-7.6).

Sits between COSMOS (target FLATSAT_RF) and the radio link:
- downlink: RF frames from the spacecraft are checked (CRC, ID) and, if the link delivers them, their space packets
  go to COSMOS
- uplink: telecommands from COSMOS wait in a queue and go up in contact as TC frames; during contact a HAIL frame
  every 20 s tells the spacecraft the ground is listening (its queued data then comes down)
- contact: ORBITAL from 42's truth (elevation above the mask), or ALWAYS, NEVER, CYCLE for tests and demos
- link model (ORBITAL): RSSI and SNR from the slant range, frame loss from the SNR (ground/flatsat_rf.py), plus a
  configurable random loss; GS_STATUS reports it all once a second, with the next pass predicted from 42's truth
- GS_* commands (MID 0x1A40) are executed here and never uplinked

Software-in-the-loop: the link emulator talks UDP to the HIL bridge (frames to the OBC on 9020 with 4 bytes of
RSSI/SNR metadata in front, frames from it on 9021). With hardware, a ground modem (ICD 7.5) replaces that leg.
"""
import argparse
import datetime
import heapq
import math
import pathlib
import random
import select
import socket
import struct
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import flatsat_icd as icd  # noqa: E402
import flatsat_rf as rf  # noqa: E402
import orbit  # noqa: E402

GS_MID = icd.CMD["GS_NOOP"][0]
HAIL_PERIOD_S = 20.0
QUEUE_MAX = 32
QUEUE_EXPIRY_S = 900.0
DUTY_BUDGET = 0.05              # ground transmitter design budget (legal limit 10 %)
NOMINAL_RANGE_KM = 800.0        # link model outside ORBITAL mode
PREDICT_PERIOD_S = 300.0
J2000 = datetime.datetime(2000, 1, 1, 12, 0, 0)
TRUTH_FIELDS = [("pos_n", 3), ("vel_n", 3), ("svb", 3), ("bvb", 3), ("hvb", 3), ("wn", 3), ("qn", 4),
                ("pos_w", 3), ("vel_w", 3)]


def clamp(x, lo, hi):
    return max(lo, min(hi, int(x)))


def parse_truth(d):
    y, doy, mo, day, hh, mm = struct.unpack(">6h", d[:12])
    if y == 0:
        return None
    ss = struct.unpack(">d", d[12:20])[0]
    t, off = {"utc": datetime.datetime(y, mo, day, hh, mm) + datetime.timedelta(seconds=ss)}, 20
    for name, n in TRUTH_FIELDS:
        t[name] = list(struct.unpack(f">{n}d", d[off:off + 8 * n]))
        off += 8 * n
    return t


class GroundStation:
    def __init__(self, a):
        self.a = a
        self.site = orbit.GroundStation(a.lat, a.lon, a.alt, a.mask)
        self.mode = icd.CONTACT_MODE[a.mode.upper()]
        self.cycle_on, self.cycle_off = a.cycle_on, a.cycle_off
        self.mode_since = time.monotonic()
        self.loss = a.loss
        self.corrupt = 0
        self.rng = random.Random(a.seed)

        self.truth = None
        self.look = (-90.0, 0.0, 0.0)
        self.passes = []          # (aos_s, los_s, max_el) relative to pass_epoch (sim seconds)
        self.pass_epoch = None
        self.next_predict = 0.0

        self.contact = False
        self.queue = []           # (enqueued monotonic, TC packet)
        self.up_counter = 0
        self.radio_free_at = 0.0
        self.airtime = []         # (monotonic, seconds) of ground transmissions in the last hour
        self.next_hail = 0.0
        self.last_rssi, self.last_snr = -140, 0  # -140 dBm: nothing received yet (ICD GS_STATUS)
        self.stats = dict(DOWN_FRAMES=0, DOWN_LOST=0, DOWN_NO_CONTACT=0, DOWN_REJECTED=0, UP_FRAMES=0, UP_DROPPED=0)
        self.in_flight = []       # (deliver at monotonic, seq, action, frame): frames still on the air
        self.flight_seq = 0
        self.status_seq = 0

        self.truth_rx = self._bind(a.truth_port)
        self.down_rx = self._bind(a.down_port)
        self.tc_rx = self._bind(a.tc_port)
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    @staticmethod
    def _bind(port):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", port))
        return s

    def log(self, msg):
        print(f"[gs {time.strftime('%H:%M:%S')}] {msg}", flush=True)

    def send(self, data, host, port):
        try:
            self.tx.sendto(data, (host, port))
        except OSError:
            pass  # the other end isn't up yet

    # ---- Time and geometry ----

    def sim_seconds(self):
        return (self.truth["utc"] - J2000).total_seconds() if self.truth else 0.0

    def on_truth(self, data):
        t = parse_truth(data)
        if t is None:
            return
        self.truth = t
        self.look = self.site.look(t["pos_w"])
        if time.monotonic() >= self.next_predict:
            p = orbit.Predictor(t["pos_n"], t["vel_n"], t["pos_w"], t["vel_w"])
            self.passes, self.pass_epoch = p.passes(self.site), self.sim_seconds()
            self.next_predict = time.monotonic() + PREDICT_PERIOD_S

    def next_pass(self):
        """(seconds to AOS, duration s, max elevation) of the next pass not yet over, or None."""
        if self.pass_epoch is None:
            return None
        now = self.sim_seconds() - self.pass_epoch
        for aos, los, max_el in self.passes:
            if los > now:
                return max(0.0, aos - now), los - aos, max_el
        return None

    def in_contact(self):
        if self.mode == icd.CONTACT_MODE["ALWAYS"]:
            return True
        if self.mode == icd.CONTACT_MODE["NEVER"]:
            return False
        if self.mode == icd.CONTACT_MODE["CYCLE"]:
            period = self.cycle_on + self.cycle_off
            return period > 0 and (time.monotonic() - self.mode_since) % period < self.cycle_on
        return self.truth is not None and self.look[0] >= self.site.mask

    def link(self):
        """(rssi dBm, snr dB, success probability) of a frame now."""
        rng = self.look[2] if self.mode == icd.CONTACT_MODE["ORBITAL"] else NOMINAL_RANGE_KM
        rssi, snr, p = rf.link(rng)
        return rssi, snr, p * (1.0 - self.loss)

    # ---- Downlink ----

    def on_air(self, airtime_s, action, frame):
        """A frame is only complete once its last symbol is on the air: deliver it airtime_s from now."""
        self.flight_seq += 1
        heapq.heappush(self.in_flight, (time.monotonic() + airtime_s, self.flight_seq, action, frame))

    def land(self):
        while self.in_flight and self.in_flight[0][0] <= time.monotonic():
            _, _, action, frame = heapq.heappop(self.in_flight)
            action(frame)

    def on_downlink(self, frame):
        self.on_air(rf.airtime_s(len(frame)), self.receive_downlink, frame)

    def receive_downlink(self, frame):
        try:
            ftype, _counter, data = rf.parse(frame)
        except rf.FrameError:
            self.stats["DOWN_REJECTED"] += 1
            return
        if not self.contact:
            self.stats["DOWN_NO_CONTACT"] += 1
            return
        rssi, snr, p = self.link()
        if self.rng.random() >= p:
            self.stats["DOWN_LOST"] += 1
            return
        self.last_rssi, self.last_snr = clamp(round(rssi), -32768, 32767), clamp(round(snr * 4), -128, 127)
        self.stats["DOWN_FRAMES"] += 1
        if data and ftype in (rf.TM, rf.BEACON):
            self.send(data, self.a.cosmos_host, self.a.tm_port)

    # ---- Uplink ----

    def duty(self):
        now = time.monotonic()
        self.airtime = [(t, s) for t, s in self.airtime if now - t < 3600]
        return sum(s for _, s in self.airtime) / 3600.0

    def transmit(self, frame_type, data=b""):
        """Sends one frame towards the spacecraft if the ground radio is free and within budget."""
        now = time.monotonic()
        frame = rf.build(frame_type, self.up_counter, data)
        t_air = rf.airtime_s(len(frame))
        if now < self.radio_free_at or self.duty() + t_air / 3600 > DUTY_BUDGET:
            return False
        self.up_counter = (self.up_counter + 1) & 0xFFFF
        self.radio_free_at = now + t_air
        self.airtime.append((now, t_air))
        self.stats["UP_FRAMES"] += 1
        if self.corrupt > 0:
            self.corrupt -= 1
            frame = frame[:-1] + bytes([frame[-1] ^ 0xFF])
            self.log(f"corrupted the CRC of uplink frame {self.up_counter - 1} (fault injection)")
        rssi, snr, p = self.link()
        if self.rng.random() < p:  # the uplink sees the same link
            meta = struct.pack(">hbB", clamp(round(rssi), -32768, 32767), clamp(round(snr * 4), -128, 127), 0)
            self.on_air(t_air, lambda f: self.send(f, self.a.rf_host, self.a.up_port), meta + frame)
        return True

    def on_telecommand(self, pkt):
        try:
            name, args = icd.parse_command(pkt)
        except (ValueError, struct.error) as e:
            self.log(f"telecommand from COSMOS rejected: {e}")
            return
        if icd.CMD[name][0] == GS_MID:
            self.ground_command(name, args)
            return
        if len(self.queue) >= QUEUE_MAX:
            self.queue.pop(0)
            self.stats["UP_DROPPED"] += 1
        self.queue.append((time.monotonic(), pkt))
        self.log(f"{name} queued for uplink ({len(self.queue)} waiting{'' if self.contact else ', no contact'})")

    def ground_command(self, name, args):
        if name == "GS_SET_CONTACT_MODE":
            mode = args["MODE"]
            if mode not in icd.CONTACT_MODE.values():
                self.log(f"GS_SET_CONTACT_MODE: unknown mode {mode}")
                return
            self.mode, self.mode_since = mode, time.monotonic()
            self.cycle_on, self.cycle_off = args["ON_TIME"], args["OFF_TIME"]
        elif name == "GS_SET_LOSS":
            self.loss = min(args["LOSS"], 1000) / 1000.0
        elif name == "GS_CORRUPT_NEXT":
            self.corrupt = args["COUNT"]
        elif name == "GS_FLUSH_QUEUE":
            self.stats["UP_DROPPED"] += len(self.queue)
            self.queue.clear()
        self.log(f"{name} {args}")

    # ---- Periodic work ----

    def service(self):
        now = time.monotonic()
        contact = self.in_contact()
        if contact != self.contact:
            self.contact = contact
            el = self.look[0]
            self.log(("AOS" if contact else "LOS") + (f": elevation {el:.1f} deg, range {self.look[2]:.0f} km"
                                                      if self.mode == icd.CONTACT_MODE["ORBITAL"] else ""))
            self.next_hail = now
        if not self.contact:
            return
        while self.queue and now - self.queue[0][0] >= QUEUE_EXPIRY_S:
            self.queue.pop(0)
            self.stats["UP_DROPPED"] += 1
        if now >= self.next_hail:
            if self.transmit(rf.HAIL):
                self.next_hail = now + HAIL_PERIOD_S
        elif self.queue:
            if self.transmit(rf.TC, self.queue[0][1]):
                self.queue.pop(0)

    def status(self):
        nxt = self.next_pass()
        pkt = icd.build_telemetry(
            "GS_STATUS", seconds=int(self.sim_seconds()), seq=self.status_seq,
            CONTACT=int(self.contact), CONTACT_MODE=self.mode, LAST_SNR=self.last_snr, LAST_RSSI=self.last_rssi,
            UP_QUEUE=len(self.queue), ELEVATION=self.look[0], AZIMUTH=self.look[1], RANGE=self.look[2],
            NEXT_AOS=0 if self.contact else (int(nxt[0]) if nxt else 0xFFFFFFFF),
            NEXT_PASS_DURATION=int(nxt[1]) if nxt else 0, NEXT_MAX_ELEVATION=int(nxt[2] * 10) if nxt else 0,
            LOSS_RATE=int(self.loss * 1000), DUTY_CYCLE=int(self.duty() * 1000), **self.stats)
        self.status_seq += 1
        self.send(pkt, self.a.cosmos_host, self.a.tm_port)

    def run(self):
        self.log(f"ground station at {self.a.lat:.4f}, {self.a.lon:.4f}, mask {self.a.mask} deg, contact "
                 f"{self.a.mode.upper()}, loss {self.loss * 100:.1f} %; COSMOS {self.a.cosmos_host}:{self.a.tm_port}, "
                 f"spacecraft {self.a.rf_host}:{self.a.up_port}")
        next_status = time.monotonic()
        socks = [self.truth_rx, self.down_rx, self.tc_rx]
        while True:
            ready, _, _ = select.select(socks, [], [], 0.01)
            for s in ready:
                data = s.recv(4096)
                if s is self.truth_rx:
                    self.on_truth(data)
                elif s is self.down_rx:
                    self.on_downlink(data)
                else:
                    self.on_telecommand(data)
            self.land()
            self.service()
            if time.monotonic() >= next_status:
                self.status()
                next_status += 1.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", default="orbital", choices=["orbital", "always", "never", "cycle"])
    ap.add_argument("--cycle-on", type=int, default=60, help="CYCLE: contact length, s")
    ap.add_argument("--cycle-off", type=int, default=60, help="CYCLE: gap, s")
    ap.add_argument("--loss", type=float, default=0.0, help="random frame loss, 0..1")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--lat", type=float, default=40.4427, help="ESA ESAC, Villafranca del Castillo (ICD 7.4)")
    ap.add_argument("--lon", type=float, default=-3.9529)
    ap.add_argument("--alt", type=float, default=650.0, help="m")
    ap.add_argument("--mask", type=float, default=10.0, help="elevation mask, deg")
    ap.add_argument("--truth-port", type=int, default=9032)
    ap.add_argument("--down-port", type=int, default=9021, help="RF frames from the spacecraft")
    ap.add_argument("--rf-host", default="nos-fsw", help="HIL bridge")
    ap.add_argument("--up-port", type=int, default=9020)
    ap.add_argument("--cosmos-host", default="cosmos")
    ap.add_argument("--tc-port", type=int, default=9030)
    ap.add_argument("--tm-port", type=int, default=9031)
    GroundStation(ap.parse_args()).run()


if __name__ == "__main__":
    main()
