#!/usr/bin/env python3
"""Restarts a real OBC (the Pico) over the umbilical and waits for it to be back, inside the SIL container.

A Linux OBC starts with each test; a Pico keeps running between tests, so a HIL test restarts it first
(sil/start_obc.sh) and every test starts from a freshly booted OBC. OBC_REBOOT goes to the bridge's
umbilical port; the restart shows in the bridge's log as the Pico's USB port going away, coming back and the
link coming up (tools/pico/attach.sh re-attaches the port to WSL meanwhile). Works whether telemetry goes to
the tests or to COSMOS. Prints the bridge log's byte offset where the restart began.

    sil/obc_restart.py [TIMEOUT_S]
"""
import os
import pathlib
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "ground"))
import flatsat_icd as icd  # noqa: E402

REBOOT_MAGIC = 0x0B0075ED
RESEND_S = 15.0  # a restart takes about 8 s; if it hasn't begun by then, the command was lost
timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
log = pathlib.Path(os.environ["SIL_LOG_DIR"]) / "bridge.log"

t0 = time.monotonic()
while not log.exists() or b"MCU link up" not in log.read_bytes():
    if time.monotonic() - t0 > timeout:
        sys.exit(f"[restart] no link to the OBC within {timeout:.0f} s (see {log})")
    time.sleep(0.2)

start = log.stat().st_size
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
next_send = time.monotonic()
while time.monotonic() - t0 < timeout:
    new = log.read_bytes()[start:]
    lost = new.find(b"serial device lost")
    if lost >= 0:
        if new.find(b"MCU link up", lost) >= 0:
            print(f"[restart] OBC restarted {time.monotonic() - t0:.1f} s after OBC_REBOOT", file=sys.stderr)
            print(start + lost)
            sys.exit(0)
    elif time.monotonic() >= next_send:
        tx.sendto(icd.build_command("OBC_REBOOT", MAGIC=REBOOT_MAGIC), ("127.0.0.1", 9010))
        next_send = time.monotonic() + RESEND_S
    time.sleep(0.2)
sys.exit(f"[restart] the OBC didn't restart within {timeout:.0f} s (see {log})")
