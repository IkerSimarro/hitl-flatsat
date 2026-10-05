"""RF frames (ICD 7.2), LoRa airtime (ICD 7.3) and the link model of the ground station (ICD 7.4).

The Python twin of firmware/common/src/fs_rf.c; tests/unit/test_rf.py checks both against the same vectors.
"""
import math
import struct

SCID = 0x01
VERSION = 0
HDR_LEN = 4
CRC_LEN = 2
MAX_FRAME = 255
MAX_DATA = MAX_FRAME - HDR_LEN - CRC_LEN

TM, TC, BEACON, HAIL = 0, 1, 2, 3  # frame types (ICD enum rf_frame_type)


class FrameError(ValueError):
    pass


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build(frame_type: int, counter: int, data: bytes = b"") -> bytes:
    if len(data) > MAX_DATA:
        raise FrameError(f"{len(data)} bytes don't fit in one RF frame (max {MAX_DATA})")
    head = bytes([(VERSION << 6) | ((frame_type & 3) << 4), SCID]) + struct.pack(">H", counter & 0xFFFF)
    body = head + data
    return body + struct.pack(">H", crc16(body))


def parse(frame: bytes):
    """Returns (type, counter, data); raises FrameError with the reason (length, crc, id)."""
    if not HDR_LEN + CRC_LEN <= len(frame) <= MAX_FRAME:
        raise FrameError("length")
    if struct.unpack(">H", frame[-2:])[0] != crc16(frame[:-2]):
        raise FrameError("crc")
    if frame[0] >> 6 != VERSION or frame[0] & 0x0F or frame[1] != SCID:
        raise FrameError("id")
    return (frame[0] >> 4) & 3, struct.unpack(">H", frame[2:4])[0], frame[HDR_LEN:-CRC_LEN]


def airtime_s(length: int, sf=7, bw_hz=125e3, cr=1, preamble=8, crc_on=True, implicit=False) -> float:
    """LoRa time on air (Semtech AN1200.13); the defaults are the FlatSat's radio settings (ICD 7.1)."""
    t_sym = (2 ** sf) / bw_hz
    de = 1 if (sf >= 11 and bw_hz <= 125e3) else 0
    num = 8 * length - 4 * sf + 28 + 16 * crc_on - 20 * implicit
    n_payload = 8 + max(math.ceil(num / (4 * (sf - 2 * de))) * (cr + 4), 0)
    return (preamble + 4.25) * t_sym + n_payload * t_sym


# ---- Link model (ICD 7.4) ----

FREQ_MHZ = 869.525
SAT_EIRP_DBM = 22.0       # flight-like spacecraft transmitter, 0 dBi patch
GS_GAIN_DBI = 12.0        # Yagi at the ground station
NOISE_FIGURE_DB = 6.0
SF7_DEMOD_SNR_DB = -7.5   # SX1262 demodulation limit at SF7


def link(range_km: float):
    """Predicted (RSSI dBm, SNR dB, success probability) for one frame at this slant range.

    The FlatSat's real radios sit on a desk at 2 dBm; this models what the same frames would see from orbit, so
    the RSSI/SNR numbers and the losses at low elevation are those of a flight link."""
    fspl = 20 * math.log10(max(range_km, 0.001)) + 20 * math.log10(FREQ_MHZ) + 32.44
    rssi = SAT_EIRP_DBM + GS_GAIN_DBI - fspl
    noise = -174 + 10 * math.log10(125e3) + NOISE_FIGURE_DB
    snr = rssi - noise
    p_ok = 1.0 / (1.0 + math.exp(-(snr - SF7_DEMOD_SNR_DB) / 1.0))
    return rssi, snr, p_ok
