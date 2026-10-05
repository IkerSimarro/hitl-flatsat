#!/usr/bin/env python3
"""Sends one FlatSat command to the OBC over the umbilical (bridge UDP port 9010), from inside the SIL container.

    sil/obc_cmd.py OBC_SET_AUTO_MODES STATE=0
    sil/obc_cmd.py OBC_SET_MODE MODE=4
"""
import pathlib
import socket
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "ground"))
import flatsat_icd as icd  # noqa: E402

if len(sys.argv) < 2 or sys.argv[1] not in icd.CMD:
    sys.exit(f"usage: {sys.argv[0]} COMMAND [FIELD=VALUE...]; commands: {', '.join(icd.CMD)}")
args = {k: float(v) if "." in v else int(v, 0) for k, v in (a.split("=", 1) for a in sys.argv[2:])}
socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(icd.build_command(sys.argv[1], **args), ("127.0.0.1", 9010))
