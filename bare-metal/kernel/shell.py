#!/usr/bin/env python3
"""Type commands to the running console over the USB network.

    python3 shell.py              a prompt (or: make shell); try "help"
    python3 shell.py peek 0xff2b0000 4     one command, then exit

Each command goes to UDP port 5002 at 192.168.36.1 as a 4-byte number then
the text. The answer comes back in pieces: the same number, a piece number
(2 bytes), a "last" flag (1 byte), then text. If an answer doesn't arrive
whole we ask again with the same number, and the console resends its saved
answer instead of running the command twice.
"""
import os
import random
import socket
import struct
import sys

CONSOLE = ("192.168.36.1", 5002)
HISTORY = os.path.expanduser("~/.r36s_shell_history")


class Shell:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.5)
        self.seq = random.getrandbits(32)

    def run(self, line, tries=6):
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        packet = struct.pack("<I", self.seq) + line.encode()
        for _ in range(tries):
            self.sock.sendto(packet, CONSOLE)
            pieces, last = {}, None
            try:
                while last is None or len(pieces) <= last:
                    data, _ = self.sock.recvfrom(2000)
                    if len(data) < 7:
                        continue
                    seq, part, is_last = struct.unpack("<IHB", data[:7])
                    if seq != self.seq:
                        continue   # a late answer to an earlier try
                    pieces[part] = data[7:]
                    if is_last:
                        last = part
            except socket.timeout:
                continue
            return b"".join(pieces[i] for i in range(last + 1)).decode(errors="replace")
        return None


def main():
    shell = Shell()
    if len(sys.argv) > 1:
        answer = shell.run(" ".join(sys.argv[1:]))
        if answer is None:
            sys.exit("shell: no answer from the console (cable in, console on?)")
        print(answer, end="")
        return

    import readline   # arrow keys and history at the prompt
    try:
        readline.read_history_file(HISTORY)
    except OSError:
        pass
    print("R36S shell at %s. help for commands, ctrl-D to leave." % CONSOLE[0])
    while True:
        try:
            line = input("r36s> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        answer = shell.run(line)
        print("(no answer from the console)" if answer is None else answer, end="")
    try:
        readline.write_history_file(HISTORY)
    except OSError:
        pass


if __name__ == "__main__":
    main()
