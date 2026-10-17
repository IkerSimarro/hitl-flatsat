#!/usr/bin/env python3
"""Shows what a FlatSat board says on its USB port, without NOS3: for bringing the boards up one by one.

    tools/pico/console.py [DEVICE] [--seconds N] [--command "NAME ARG=VALUE ..."]...

DEVICE defaults to /dev/ttyACM0. The board is recognised by its USB name (hardware/PINOUT.md):
- ADCS or EPS node: its log, as plain text (kept from start-up until a terminal opens the port)
- OBC: it speaks the HIL link (ICD 8). The console answers its heartbeats, so the OBC sees the link up and sends
  its log, starting with the lines it logged at start-up (the probes for its CAN board, radio and display). There
  is no NOS3 behind it, so the console answers its I2C, SPI and CAN requests with a bus error at once, as the bridge
  does for a failed device; left waiting for their timeouts they would hold up its other tasks. Its simulated
  sensors therefore fail, as expected; a summary counts the frames every 10 s.
  --command sends it a command once the link is up, by its ICD name, with values as numbers or enum names, e.g.
  --command "OBC_SET_MODE MODE=TEST" --command "ADCS_PHYS_WHEEL_TEST CTRL_MODE=SPEED SETPOINT=300"
- ground modem: its log, the frames its radio receives (type, length, RSSI and SNR), and changes in its radio state
  and dropped frames (from its heartbeat)

Ctrl-C stops it; --seconds stops it after N seconds. If the board restarts, the port goes away and the console
says so (a watchdog reset, for example).
"""
import argparse
import collections
import os
import pathlib
import select
import sys
import time
import tty

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "nos3" / "components" / "hil_bridge" / "support"))
sys.path.insert(0, str(ROOT / "ground"))
import flatsat_icd as icd  # noqa: E402
import flatsat_rf as rf  # noqa: E402
import hil_link  # noqa: E402

RF_TYPES = {rf.TM: "TM", rf.TC: "TC", rf.BEACON: "BEACON", rf.HAIL: "HAIL"}
REPLY = {hil_link.I2C_TXN: hil_link.I2C_RSP, hil_link.SPI_TXN: hil_link.SPI_RSP, hil_link.CAN_TXN: hil_link.CAN_RSP}
BUS_ERROR = 1  # hil_link status


def command(text):
    """ "OBC_SET_MODE MODE=TEST" -> the command packet; values are numbers or names from the ICD's enums."""
    name, *args = text.split()
    if name not in icd.CMD:
        sys.exit(f"[console] unknown command {name}")
    values = {}
    for a in args:
        key, _, value = a.partition("=")
        try:
            values[key] = int(value, 0)
        except ValueError:
            enums = [getattr(icd, key, None)] + [v for k, v in vars(icd).items() if k.isupper() and isinstance(v, dict)]
            match = [e[value] for e in enums if isinstance(e, dict) and value in e]
            if not match:
                sys.exit(f"[console] {name}: {key}={value} is neither a number nor an ICD enum name")
            values[key] = match[0]
    return icd.build_command(name, **values)


def board_name(device):
    """The USB product name, from /dev/serial/by-id (e.g. "FlatSat OBC"), or None."""
    by_id = pathlib.Path("/dev/serial/by-id")
    real = os.path.realpath(device)
    for link in by_id.glob("*") if by_id.exists() else []:
        if os.path.realpath(link) == real:
            return link.name  # usb-HITL_FlatSat_FlatSat_OBC_<serial>-if00
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", nargs="?", default="/dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--command", action="append", default=[], help="OBC: a command to send once the link is up")
    a = ap.parse_args()
    commands = [command(c) for c in a.command]

    name = board_name(a.device) or ""
    kind = ("node" if "_node_" in name else "modem" if "_modem_" in name else "obc" if "_OBC_" in name else None)
    if kind is None:
        sys.exit(f"{a.device}: not a FlatSat board ({name or 'no USB name found'}); is it attached and flashed?")
    print(f"[console] {a.device}: {name.split('_FlatSat_', 1)[-1].rsplit('_', 1)[0].replace('_', ' ')}", flush=True)

    fd = os.open(a.device, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    dec = hil_link.Decoder()
    counts = collections.Counter()
    radio_ok, dropped = None, 0
    link_at = None  # when the OBC's link came up
    t0 = time.monotonic()
    next_summary = t0 + 10.0
    try:
        while a.seconds is None or time.monotonic() - t0 < a.seconds:
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    data = os.read(fd, 4096)
                except OSError as e:
                    sys.exit(f"[console] the port went away after {time.monotonic() - t0:.1f} s ({e.strerror}): "
                             "the board restarted or was unplugged")
                if kind == "node":
                    sys.stdout.write(data.decode(errors="replace"))
                    sys.stdout.flush()
                    continue
                for f in dec.feed(data):
                    if f.type == hil_link.LOG:
                        print(f.payload.decode(errors="replace"), flush=True)
                    elif f.type in REPLY and kind == "obc":  # a simulated device's bus: no NOS3 to answer
                        os.write(fd, hil_link.encode(hil_link.Frame(REPLY[f.type], bus=f.bus, seq=f.seq,
                                                                    status=BUS_ERROR, addr=f.addr)))
                        counts[hil_link.TYPE_NAMES[f.type]] += 1
                    elif f.type == hil_link.HEARTBEAT and kind == "obc":
                        os.write(fd, hil_link.encode(hil_link.Frame(hil_link.HEARTBEAT, seq=f.seq)))  # echo: link up
                        link_at = link_at or time.monotonic()
                    elif f.type == hil_link.RF_RX:  # the modem received a frame
                        rssi = (f.addr & 0xFFFF) - (0x10000 if f.addr & 0x8000 else 0)
                        snr = ((f.addr >> 16) & 0xFF) - (0x100 if f.addr & 0x800000 else 0)
                        try:
                            what = RF_TYPES.get(rf.parse(f.payload)[0], "?")
                        except rf.FrameError as e:
                            what = f"bad frame ({e})"
                        print(f"[console] received {what}, {len(f.payload)} bytes, RSSI {rssi} dBm, "
                              f"SNR {snr / 4:.2f} dB", flush=True)
                    elif f.type == hil_link.HEARTBEAT:  # the modem: radio state and frames dropped
                        if (f.status == 0) != radio_ok:
                            radio_ok = f.status == 0
                            print(f"[console] modem radio {'working' if radio_ok else 'not answering'}", flush=True)
                        if f.addr != dropped:
                            print(f"[console] modem has dropped {f.addr} frame(s)", flush=True)
                            dropped = f.addr
                    else:
                        counts[hil_link.TYPE_NAMES.get(f.type, hex(f.type))] += 1
            if commands and link_at is not None and time.monotonic() - link_at > 1.0:
                os.write(fd, hil_link.encode(hil_link.Frame(hil_link.CI_PKT, payload=commands.pop(0))))
                print(f"[console] sent {a.command[len(a.command) - len(commands) - 1]}", flush=True)
                link_at = time.monotonic()  # the next one a second later
            if kind == "obc" and time.monotonic() >= next_summary:
                next_summary += 10.0
                print(f"[console] in the last 10 s: {dict(counts) or 'no other frames'}", flush=True)
                counts.clear()
    except KeyboardInterrupt:
        pass
    print(f"[console] {time.monotonic() - t0:.0f} s, no restart seen", flush=True)


if __name__ == "__main__":
    main()
