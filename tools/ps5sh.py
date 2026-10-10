#!/usr/bin/env python3
# Steam-Link-PS5 - Run a command on the console through shsrv.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a command on a jailbroken PS5 through shsrv (telnet, port 2323)."""
import socket, sys, time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

def negotiate(data, sock):
    """Strip telnet control sequences, refusing every option offered."""
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] != IAC:
            out.append(data[i]); i += 1; continue
        if i + 1 >= len(data):
            break
        cmd = data[i + 1]
        if cmd in (DO, DONT) and i + 2 < len(data):
            sock.sendall(bytes([IAC, WONT, data[i + 2]])); i += 3
        elif cmd in (WILL, WONT) and i + 2 < len(data):
            sock.sendall(bytes([IAC, DONT, data[i + 2]])); i += 3
        elif cmd == SB:
            j = data.find(bytes([IAC, SE]), i)
            i = len(data) if j < 0 else j + 2
        else:
            i += 2
    return bytes(out)

def run(host, command, port=2323, settle=1.5, idle=2.0):
    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(settle)
    text = bytearray()
    # Let the server finish negotiating and print its prompt
    deadline = time.time() + settle
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            text += negotiate(chunk, s)
        except socket.timeout:
            break
    s.sendall(command.encode() + b"\n")
    s.settimeout(idle)
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            text += negotiate(chunk, s)
        except socket.timeout:
            break
    s.close()
    return text.decode("utf-8", "replace")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: ps5sh.py <host> <command...>")
    print(run(sys.argv[1], " ".join(sys.argv[2:])), end="")
