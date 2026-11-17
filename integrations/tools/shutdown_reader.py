#!/usr/bin/env python3
"""Drain an owned reader after the application has closed its graph files."""
import argparse
import socket
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("socket")
args=parser.parse_args()
with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
    client.settimeout(30)
    client.connect(args.socket)
    client.sendall(b"shutdown")
    reply=b""
    while not reply.endswith(b"\n"):
        part=client.recv(32)
        if not part:raise RuntimeError("reader closed without a complete response")
        reply+=part
        if len(reply)>32:raise RuntimeError("invalid reader response")
    result=int(reply)
    if result:raise SystemExit("reader not shut down: "+str(result)+" (release collections/readers first)")
print("Reader drained and NVFUSE stopped")
