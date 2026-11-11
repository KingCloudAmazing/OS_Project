"""Stand-in for the xv6 serial console, for bridge failure tests: python fake_guest.py MODE

ok      boots, then answers each command in small fragments (partial reads)
noboot  never reaches the shell prompt
hang    echoes the command but never finishes it
die     exits as soon as a command arrives (QEMU termination)
garbage answers without the command echo and with unexpected text
"""
import sys
import time

mode = sys.argv[1]


def out(text, pieces=1):
    step = max(1, len(text) // pieces)
    for i in range(0, len(text), step):
        sys.stdout.write(text[i:i + step])
        sys.stdout.flush()
        time.sleep(0.02)


if mode == "noboot":
    out("Booting from Hard Disk..\r\n")
    time.sleep(60)
out("xv6...\r\ninit: starting sh\r\n$ ")
for line in sys.stdin:
    typed = line.rstrip("\r\n")
    marker = typed.rpartition(" ; echo ")[2]
    if mode == "die":
        sys.exit(1)
    if mode != "garbage":
        out(typed + "\r\n")
    if mode == "hang":
        time.sleep(60)
    out("first line\r\nsecond $ line\r\n" if mode == "ok" else "\x1b[0m??? noise\r\n", pieces=9)
    out(f"{marker}\r\n$ ")
