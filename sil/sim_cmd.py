#!/usr/bin/env python3
"""Sends a command to a NOS3 simulator over the command bus (fault injection and simulator control).

    sil/sim_cmd.py imu-command DISABLE
    sil/sim_cmd.py imu-command ENABLE

Uses NOS3's command bus bridge (nos3-sim-cmdbus-bridge, TCP 12020, newline-terminated JSON), which the
SIL environment starts. Node names are in nos3/cfg/sims/sc-1-nos3-simulator.xml, e.g. imu-command,
eps-command, rw0-command. Run it inside the SIL container (sil/sil.sh or docker exec).
"""
import json
import socket
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)
node, cmd = sys.argv[1], sys.argv[2]
with socket.create_connection(("127.0.0.1", 12020), timeout=5) as s:
    s.sendall((json.dumps({"node": node, "cmd": cmd}) + "\n").encode())
