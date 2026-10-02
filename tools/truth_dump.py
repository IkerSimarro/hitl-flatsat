#!/usr/bin/env python3
"""Print 42 truth packets (truth42sim UDP stream, port 5111) for debugging."""
import socket, struct, sys, time
names = ["pos_n", "vel_n", "svb", "bvb", "hvb", "wn", "qn", "pos_w", "vel_w", "acc_b", "gyro_b", "rw_h"]
sizes = [3, 3, 3, 3, 3, 3, 4, 3, 3, 3, 3, 3]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 5111))
for n in range(int(sys.argv[1]) if len(sys.argv) > 1 else 1):
    d = s.recv(1024)
    y, doy, mo, day, hh, mm = struct.unpack(">6h", d[:12])
    ss = struct.unpack(">d", d[12:20])[0]
    print(f"len {len(d)}  {y}-{mo:02d}-{day:02d} {hh:02d}:{mm:02d}:{ss:06.3f}")
    off = 20
    for nm, sz in zip(names, sizes):
        v = struct.unpack(f">{sz}d", d[off:off + 8 * sz])
        off += 8 * sz
        print(f"  {nm:7s}", " ".join(f"{x: .6e}" for x in v))
    time.sleep(2)
