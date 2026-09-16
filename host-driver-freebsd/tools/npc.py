#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""npc.py — run one command on the NPU's serial console, from the FreeBSD host.

The CN9130 NPU's console is wired to the x86 host's THIRD UART. That is
/dev/ttyS2 on Linux (see ../../host-driver-linux/tools/npc.sh) and /dev/cuau2 on
FreeBSD; both are the same 16550 at I/O port 0x3e8, which is how you confirm you
have the right node:

    dmesg | grep -E 'uart[0-9]'
    uart2: <16550 or compatible> port 0x3e8-0x3ef irq 3 on acpi0

Usage:
    python3 npc.py 'md5sum /opt/dp/dp_fwd'
    python3 npc.py 'cat /proc/cmdline' 5
    NPC_DEV=/dev/cuau2 NPC_BAUD=115200 python3 npc.py 'uname -a'

The NPU answers at a BusyBox root prompt ("/ # ") with no login, so this is a
management path that works even when mvmgmt0 does not -- which is the case on an
NPU running this kit's payload, where the pcinet link never reaches ESTABLISHED.

WHY THIS IS PYTHON AND NOT A THREE-LINE SHELL SCRIPT
----------------------------------------------------
The Linux tool is a shell script because on Linux `stty -F DEV ...` persists
after its descriptor closes, so a following `cat DEV` inherits the line
settings. On FreeBSD and macOS IT DOES NOT: the termios settings are dropped
when the stty descriptor closes, and the subsequent `cat` runs at whatever rate
the port had before. The result is not an error -- it is either garbage, or, in
canonical mode, NOTHING AT ALL, because wrong-rate bytes rarely contain a
newline and the line discipline buffers them forever.

A silent line is indistinguishable from a dead NPU. A direct transcription of
npc.sh to FreeBSD therefore produces confident, repeatable, and completely false
evidence that the NPU's console is unreachable -- measured on this bench across
five baud rates, while the NPU was in fact sitting at a root prompt the whole
time. Hours were spent on recovery plans for a board that was never stranded.

So: set termios with tcsetattr on the SAME descriptor used for read and write,
and put the port in raw mode so nothing waits for a newline.
"""
import os
import sys
import termios
import time

DEV = os.environ.get("NPC_DEV", "/dev/cuau2")
BAUD = os.environ.get("NPC_BAUD", "115200")


def open_console(dev, baud):
    """Open dev and apply raw termios at baud ON THAT SAME DESCRIPTOR."""
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0                                    # iflag: no input processing
    a[1] = 0                                    # oflag: no output processing
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0                                    # lflag: raw, not canonical
    a[4] = a[5] = getattr(termios, "B" + baud)  # ispeed / ospeed
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def run(cmd, wait):
    fd = open_console(DEV, BAUD)
    try:
        os.write(fd, cmd.encode() + b"\n")
        buf, end = b"", time.time() + wait
        while time.time() < end:
            try:
                chunk = os.read(fd, 4096)
            except (BlockingIOError, OSError):
                chunk = b""
            if chunk:
                buf += chunk
            else:
                time.sleep(0.05)
        return buf
    finally:
        os.close(fd)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[0])
    # Keep commands SHORT. A command long enough to wrap makes the console
    # redraw its prompt mid-echo, which any prompt matcher will read as a
    # completed command that produced no output.
    out = run(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 3.0)
    sys.stdout.write(out.decode("utf-8", "replace"))
    if not out:
        sys.stderr.write(
            "\nnpc: no bytes. Before concluding the NPU is down, confirm the\n"
            "node is the 0x3e8 UART (dmesg | grep uart) and try other rates\n"
            "with NPC_BAUD. A quiet line and a wrong rate look identical.\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
