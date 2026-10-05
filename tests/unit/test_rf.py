"""RF frame, airtime and link model checks; the frame and airtime vectors match firmware/tests/test_common.c."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "ground"))
import flatsat_rf as rf  # noqa: E402

PKT = bytes([0x0A, 0x01, 0xC0, 0x00, 0x00, 0x05, 1, 2, 3, 4, 5, 6])


class TestFrames(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(rf.crc16(b"123456789"), 0x29B1)

    def test_build_matches_c(self):
        frame = rf.build(rf.BEACON, 0x1234, PKT)
        self.assertEqual(frame[:4], bytes([0x20, 0x01, 0x12, 0x34]))
        self.assertEqual(frame[-2:], bytes([0x7F, 0x28]))  # same vector as test_common.c

    def test_round_trip_and_errors(self):
        frame = rf.build(rf.TC, 99, PKT)
        self.assertEqual(rf.parse(frame), (rf.TC, 99, PKT))
        bad = bytearray(frame)
        bad[6] ^= 1
        with self.assertRaisesRegex(rf.FrameError, "crc"):
            rf.parse(bytes(bad))
        other = bytearray(frame[:-2])
        other[1] = 2
        other += rf.crc16(bytes(other)).to_bytes(2, "big")
        with self.assertRaisesRegex(rf.FrameError, "id"):
            rf.parse(bytes(other))
        with self.assertRaisesRegex(rf.FrameError, "length"):
            rf.parse(frame[:5])
        with self.assertRaises(rf.FrameError):
            rf.build(rf.TM, 0, bytes(rf.MAX_DATA + 1))

    def test_hail(self):
        frame = rf.build(rf.HAIL, 7)
        self.assertEqual(len(frame), 6)
        self.assertEqual(rf.parse(frame), (rf.HAIL, 7, b""))


class TestAirtimeAndLink(unittest.TestCase):
    def test_airtime_matches_c(self):
        self.assertAlmostEqual(rf.airtime_s(255), 0.399616, places=6)
        self.assertAlmostEqual(rf.airtime_s(42), 0.087296, places=6)
        self.assertAlmostEqual(rf.airtime_s(6), 0.036096, places=6)

    def test_link_budget(self):
        # Zenith (400 km): strong; the 10 deg elevation mask (~1800 km): marginal but usually closes
        rssi, snr, p = rf.link(400)
        self.assertTrue(-120 < rssi < -105 and p > 0.999)
        rssi, snr, p = rf.link(1800)
        self.assertTrue(-125 < rssi < -118 and 0.5 < p < 0.99)
        self.assertLess(rf.link(5000)[2], 0.01)


if __name__ == "__main__":
    unittest.main()
