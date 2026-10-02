#!/usr/bin/env python3
"""Forwards the 42 truth stream (UDP 5111) to the ground segment host given as the argument."""
import socket
import sys

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("0.0.0.0", 5111))
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
warned = False
while True:
    data = rx.recv(2048)
    try:
        tx.sendto(data, (sys.argv[1], 5111))
    except OSError as e:  # ground segment not up yet
        if not warned:
            print("truth relay:", e, flush=True)
            warned = True
