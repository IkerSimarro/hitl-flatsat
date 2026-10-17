"""The ground station software with the ground modem (ICD 7.5): a fake modem on a pseudo-terminal."""
import argparse
import os
import pathlib
import select
import socket
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_gs as gs  # noqa: E402
import flatsat_rf as rf  # noqa: E402
from flatsat_gs import hil_link  # noqa: E402

PKT = bytes([0x0A, 0x01, 0xC0, 0x00, 0x00, 0x05, 1, 2, 3, 4, 5, 6])


class Station(gs.GroundStation):
    """Keeps its log for the checks."""

    def __init__(self, a):
        self.logs = []
        super().__init__(a)

    def log(self, msg):
        self.logs.append(msg)


class TestGroundModem(unittest.TestCase):
    def setUp(self):
        self.modem, slave = os.openpty()  # the test is the modem, on the pseudo-terminal's master side
        path = os.ttyname(slave)
        os.close(slave)
        self.cosmos = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.cosmos.bind(("127.0.0.1", 0))
        self.cosmos.settimeout(0)
        a = argparse.Namespace(mode="always", cycle_on=60, cycle_off=60, loss=0.0, seed=1, lat=40.4427, lon=-3.9529,
                               alt=650.0, mask=10.0, truth_port=0, down_port=0, tc_port=0, rf_host="127.0.0.1",
                               up_port=9, cosmos_host="127.0.0.1", tm_port=self.cosmos.getsockname()[1], modem=path)
        self.gs = Station(a)
        self.gs.step(0)  # contact (ALWAYS) starts, and with it the first HAIL

    def tearDown(self):
        for s in (self.gs.truth_rx, self.gs.down_rx, self.gs.tc_rx, self.gs.tx, self.cosmos):
            s.close()
        if self.gs.modem.fd is not None:
            os.close(self.gs.modem.fd)
        if self.modem is not None:
            os.close(self.modem)

    def from_ground(self, timeout=2.0):
        """The frames the ground station sent to the modem."""
        dec, frames, end = hil_link.Decoder(), [], time.monotonic() + timeout
        while not frames and time.monotonic() < end:
            if select.select([self.modem], [], [], 0.05)[0]:
                frames += list(dec.feed(os.read(self.modem, 4096)))
        return frames

    def to_ground(self, frame_type, addr=0, payload=b"", status=0):
        os.write(self.modem, hil_link.encode(hil_link.Frame(frame_type, status=status, addr=addr, payload=payload)))

    def cosmos_packets(self, timeout=1.0):
        pkts, end = [], time.monotonic() + timeout
        while time.monotonic() < end:
            self.gs.step(0.02)
            try:
                while True:
                    pkts.append(self.cosmos.recv(4096))
            except BlockingIOError:
                pass
        return pkts

    def test_uplink_goes_on_the_air(self):
        frames = self.from_ground()
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].type, hil_link.RF_TX)
        self.assertEqual(rf.parse(frames[0].payload)[0], rf.HAIL)
        self.assertEqual(self.gs.stats["UP_FRAMES"], 1)

    def test_downlink_with_the_measured_link(self):
        rssi, snr_q4 = -87, 25  # 6.25 dB
        self.to_ground(hil_link.RF_RX, (rssi & 0xFFFF) | (snr_q4 << 16), rf.build(rf.BEACON, 5, PKT))
        self.assertIn(PKT, self.cosmos_packets())
        self.assertEqual((self.gs.last_rssi, self.gs.last_snr), (rssi, snr_q4))
        self.assertEqual(self.gs.stats["DOWN_FRAMES"], 1)

    def test_corrupted_downlink_rejected(self):
        frame = bytearray(rf.build(rf.BEACON, 5, PKT))
        frame[6] ^= 1
        self.to_ground(hil_link.RF_RX, 0, bytes(frame))
        self.assertNotIn(PKT, self.cosmos_packets(0.3))
        self.assertEqual(self.gs.stats["DOWN_REJECTED"], 1)

    def test_modem_messages_radio_state_and_drops(self):
        self.to_ground(hil_link.LOG, payload=b"radio ready")
        self.to_ground(hil_link.HEARTBEAT, addr=2, status=1)  # BUS_ERROR: the radio isn't answering
        self.to_ground(hil_link.HEARTBEAT, addr=2, status=1)
        self.to_ground(hil_link.HEARTBEAT, addr=2, status=0)
        self.cosmos_packets(0.3)
        self.assertIn("modem: radio ready", self.gs.logs)
        self.assertEqual([m for m in self.gs.logs if "ground modem: radio" in m],
                         ["ground modem: radio not answering", "ground modem: radio working"])
        self.assertEqual(sum("dropped 2 uplink" in m for m in self.gs.logs), 1, self.gs.logs)

    def test_unplugged_modem_holds_the_uplink(self):
        self.from_ground()  # the first HAIL
        os.close(self.modem)
        self.modem = None
        self.cosmos_packets(0.3)
        self.assertIsNone(self.gs.modem.fd)
        self.assertTrue(any("lost" in m for m in self.gs.logs), self.gs.logs)
        self.gs.radio_free_at = 0.0
        self.assertFalse(self.gs.transmit(rf.HAIL))  # waits for the modem rather than losing the frame
        self.assertEqual(self.gs.stats["UP_FRAMES"], 1)


if __name__ == "__main__":
    unittest.main()
