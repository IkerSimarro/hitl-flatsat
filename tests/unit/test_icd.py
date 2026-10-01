"""Unit tests for the generated ICD artefacts (ground/flatsat_icd.py and the C header).

Run from the repo root:  python3 -m unittest discover -s tests/unit -v
"""
import pathlib
import re
import struct
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "ground"))
sys.path.insert(0, str(ROOT / "tools"))

import flatsat_icd as icd  # noqa: E402
import icd_gen  # noqa: E402

C_HEADER = (ROOT / "firmware/common/include/flatsat_icd.h").read_text()


class TestCommands(unittest.TestCase):
    def test_roundtrip_every_command(self):
        for name, (mid, fc, fmt, names) in icd.CMD.items():
            # fmt is "<" followed by one struct code per argument; floats get float values
            args = {n: (float(i + 1) if code == "f" else i + 1) for i, (n, code) in enumerate(zip(names, fmt[1:]))}
            packet = icd.build_command(name, **args)
            parsed_name, parsed_args = icd.parse_command(packet)
            self.assertEqual(parsed_name, name)
            self.assertEqual(parsed_args, args)

    def test_checksum_matches_cfs_rule(self):
        packet = icd.build_command("OBC_SET_MODE", MODE=icd.MODE["DETUMBLE"])
        x = 0
        for b in packet:
            x ^= b
        self.assertEqual(x, 0xFF)

    def test_header_fields(self):
        packet = icd.build_command("OBC_PING", seq=5, TOKEN=0xDEADBEEF)
        mid, seq, length, fc, _ = struct.unpack(">HHHBB", packet[:8])
        self.assertEqual(mid, 0x1A00)
        self.assertEqual(seq, 0xC005)
        self.assertEqual(length, len(packet) - 7)
        self.assertEqual(fc, 4)

    def test_corrupt_command_rejected(self):
        packet = bytearray(icd.build_command("OBC_REBOOT", MAGIC=0x0B0075ED))
        packet[-1] ^= 0x01
        with self.assertRaises(ValueError):
            icd.parse_command(bytes(packet))

    def test_unknown_argument_rejected(self):
        with self.assertRaises(KeyError):
            icd.build_command("OBC_SET_MODE", MODE=1, BOGUS=2)


class TestTelemetry(unittest.TestCase):
    def test_roundtrip_every_packet(self):
        for name, (mid, fmt, names) in icd.TLM.items():
            fields = {}
            for i, (n, code) in enumerate(zip(names, re.findall(r"\d+s|[a-zA-Z]", fmt[1:]))):
                if code.endswith("s"):
                    fields[n] = b"hello"
                elif code == "f":
                    fields[n] = float(i)
                else:
                    fields[n] = i % 100
            packet = icd.build_telemetry(name, seconds=814254200, subseconds=0x8000, seq=7, **fields)
            parsed_name, header, parsed = icd.parse_telemetry(packet)
            self.assertEqual(parsed_name, name)
            self.assertEqual(header, {"seq": 7, "seconds": 814254200, "subseconds": 0x8000})
            for n, v in fields.items():
                if isinstance(v, bytes):
                    self.assertEqual(parsed[n].rstrip(b"\0"), v)
                else:
                    self.assertEqual(parsed[n], v)

    def test_sizes_match_c_header(self):
        for name, (mid, fmt, _names) in icd.TLM.items():
            total = struct.calcsize(fmt) + 16
            m = re.search(rf"#define FLATSAT_{name}_LEN (\d+)", C_HEADER)
            self.assertIsNotNone(m, name)
            self.assertEqual(int(m.group(1)), total, name)
            self.assertIn(f"#define FLATSAT_{name}_MID 0x{mid:04X}", C_HEADER)

    def test_every_packet_fits_rf_link(self):
        for name, (_mid, fmt, _names) in icd.TLM.items():
            self.assertLessEqual(struct.calcsize(fmt) + 16, 249, name)

    def test_beacon_is_compact(self):
        # ICD 4: beacon at most 40 bytes so its airtime stays around 80 ms
        self.assertLessEqual(struct.calcsize(icd.TLM["BEACON"][1]) + 16, 40)


class TestCan(unittest.TestCase):
    def test_roundtrip_and_dlc(self):
        for name, (msg_type, _src, fmt, names) in icd.CAN.items():
            fields = {n: i + 1 for i, n in enumerate(names)}
            data = icd.pack_can(name, **fields)
            self.assertLessEqual(len(data), 8, name)
            self.assertEqual(icd.unpack_can(name, data), fields)
            m = re.search(rf"#define FLATSAT_CAN_{name}_DLC (\d+)", C_HEADER)
            self.assertEqual(int(m.group(1)), len(data), name)

    def test_ids_unique_per_node(self):
        ids = set()
        for name, (msg_type, src, _fmt, _names) in icd.CAN.items():
            nodes = icd.NODE.values() if src == "ANY" else [icd.NODE[src]]
            for node in nodes:
                can_id = icd.can_id(msg_type, node)
                self.assertNotIn(can_id, ids, name)
                self.assertLess(can_id, 0x800, name)
                ids.add(can_id)


class TestGenerator(unittest.TestCase):
    def test_misaligned_field_rejected(self):
        with self.assertRaises(icd_gen.IcdError):
            icd_gen.layout([{"name": "A", "type": "u8"}, {"name": "B", "type": "f32"}], "test")

    def test_keyword_field_rejected(self):
        with self.assertRaises(icd_gen.IcdError):
            icd_gen.layout([{"name": "SWITCH", "type": "u8"}], "test")

    def test_generated_files_up_to_date(self):
        data = icd_gen.load()
        for rel, gen in icd_gen.OUTPUTS.items():
            self.assertEqual((ROOT / rel).read_text(), gen(data), f"{rel} is stale; run tools/icd_gen.py")


if __name__ == "__main__":
    unittest.main()
