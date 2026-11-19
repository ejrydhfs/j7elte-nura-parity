#!/usr/bin/env python3
"""
Off-target test of the --init handshake of j7elte-gpsd.

The BCM4752 is emulated by the test itself on the master side of a PTY.
It reproduces what the real chip does on j7elte hardware: it never sends the
unsolicited BRI announce frame, ignores the 10-byte NvRamWord7 selection, and
answers the NvRamWord7 read, the 0x40/0x1a status request, the HW version
request and the DebugSfr request -- in that order and only in that order
(DebugSfr is state-gated on the NvRamWord7 exchange, as the vendor code
asserts at 0x2a6784).  The test checks that the daemon emits exactly the
bytes in the reverse-engineered tables, in the order the hardware answers.

The daemon runs under qemu-aarch64 against the cross sysroot, so this also
exercises the real termios/poll/write path.
"""
import os
import pty
import re
import select
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "j7elte-gpsd")
SYSROOT = ("/run/media/user/00000000-0000-4000-8000-000000000000/"
           "j7elte-session/toolchains/gcc-arm-10.3-2021.07-x86_64-"
           "aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc")

AUTOBAUD = b"\x80" * 16
OUT_OF_LEGACY = bytes([0xFE, 0x00, 0xFD, 0x00, 0x00, 0x55, 0xAA, 0x00, 0xFF, 0xFC])
REQ_HW_VERSION = bytes([0xFF, 0x00, 0xFD, 0x00, 0x00, 0xFC])
REQ_DEBUG_SFR = bytes([0xFF, 0x00, 0xFD, 0x1F, 0x95, 0xFC])
NV_SELECT = bytes([0xFE, 0x00, 0xFD, 0x00, 0x1B, 0x07, 0x00, 0x00, 0x00, 0xFC])
NV_READ = bytes([0xFF, 0x00, 0xFD, 0x00, 0x1B, 0xFC])
REQ_STATUS = bytes([0xFF, 0x00, 0xFD, 0x40, 0x1A, 0xFC])


def resp(cmd, sub, data=b"\x02\x03\x04\x05"):
    return bytes([0xFE, 0x00, 0xFD, cmd, sub]) + data + b"\xFC"


def read_exact(fd, n, timeout=10.0):
    buf = b""
    end = time.time() + timeout
    while len(buf) < n and time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            buf += os.read(fd, 4096)
            # the PTY master echoes nothing; drop any CR/LF noise
            buf = buf.replace(b"\r", b"").replace(b"\n", b"")
    return buf


def main():
    failures = []

    def check(name, cond, detail=""):
        print(("PASS: " if cond else "FAIL: ") + name + ((" " + detail) if detail and not cond else ""))
        if not cond:
            failures.append(name)

    tmp = tempfile.mkdtemp(prefix="gpsd-init-")
    rundir = os.path.join(tmp, "run")
    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)

    cmd = [
        "qemu-aarch64", "-L", SYSROOT, BIN,
        "--device", slave_name,
        "--baud", "921600",
        "--no-power",
        "--power-delay", "0",
        "--rundir", rundir,
        "--socket", "none",
        "--init",
        "--verbose",
    ]
    logpath = os.path.join(tmp, "daemon.log")
    log = open(logpath, "w+")
    proc = subprocess.Popen(cmd, stdout=log, stderr=log)

    try:
        got = read_exact(master, len(AUTOBAUD))
        check("autobaud burst is 16 x 0x80", got == AUTOBAUD, repr(got))

        # The real chip never announces itself; answer anyway so that the
        # daemon's announce window has something to log.
        os.write(master, resp(0x00, 0x01))

        got = read_exact(master, len(NV_SELECT))
        check("Bri_RequestNvRamWord7 selection frame", got == NV_SELECT, repr(got))
        # ... which the chip ignores.

        got = read_exact(master, len(NV_READ))
        check("NvRamWord7 read frame", got == NV_READ, repr(got))
        os.write(master, resp(0x00, 0x1B, b"\x07\x00\x00\x00"))

        got = read_exact(master, len(REQ_STATUS))
        check("status 0x40/0x1a frame", got == REQ_STATUS, repr(got))
        os.write(master, resp(0x40, 0x1A, b"\x02\x00\x10\xa4"))

        got = read_exact(master, len(REQ_HW_VERSION))
        check("Bri_RequestHwVersion frame", got == REQ_HW_VERSION, repr(got))
        os.write(master, resp(0x00, 0x00, b"\x05\x00\x02\x30"))

        got = read_exact(master, len(REQ_DEBUG_SFR))
        check("Bri_RequestDebugSfr frame", got == REQ_DEBUG_SFR, repr(got))
        os.write(master, resp(0x1F, 0x95, b"\x00\x00\x00\x00"))

        got = read_exact(master, len(OUT_OF_LEGACY))
        check("Bri_GetOutOfLegacy frame", got == OUT_OF_LEGACY, repr(got))
        os.write(master, resp(0x00, 0x01))

        time.sleep(0.4)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        log.flush()
        text = open(logpath).read()
        log.close()

    check("NvRamWord7 word decoded", "NvRamWord7 word 7 = 0x00000007" in text)
    check("status decoded", "status 0x40/0x1a = 0xa4100002" in text)
    check("HW version decoded", "BRI HW version 0x30020005" in text)
    check("DebugSfr acknowledged", "DebugSfr acknowledged" in text)
    check("link check counted", "link check: 5 of 6 exchanges answered" in text)
    check("residual blocker logged", "stopping here" in text)
    check("GetOutOfLegacy not repeated after an ack",
          text.count("Bri_GetOutOfLegacy sent") == 1)

    print("\n--- daemon log ---")
    print(text)
    print("------------------")
    print(("FAILED: %d" % len(failures)) if failures else "ALL PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
