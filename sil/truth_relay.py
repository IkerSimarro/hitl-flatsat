#!/usr/bin/env python3
"""Fans the 42 truth stream (UDP 5111) out to its consumers in the SIL environment (ICD 3.6).

    truth_relay.py HOST:PORT [HOST:PORT...]

sil_env.sh sends it to the ground station software (9032), to SIL tests and tools (5112) and, when there is one,
to the ground segment host (COSMOS, 5111). One UDP port can only have one reader, hence the relay.
"""
import socket
import sys

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("0.0.0.0", 5111))
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
dests = [(h, int(p)) for h, p in (a.rsplit(":", 1) for a in sys.argv[1:])]
warned = set()
while True:
    data = rx.recv(2048)
    for d in dests:
        try:
            tx.sendto(data, d)
        except OSError as e:  # that consumer isn't up yet
            if d not in warned:
                print(f"truth relay: {d[0]}:{d[1]}: {e}", flush=True)
                warned.add(d)
