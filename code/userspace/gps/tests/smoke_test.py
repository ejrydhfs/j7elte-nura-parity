#!/usr/bin/env python3
"""
Off-target functional test for j7elte-gpsd.

No BCM4752 hardware is involved: the "UART" is a host PTY, the "GPS_PWR_EN"
attribute is a regular file, and the daemon itself runs under qemu-aarch64
against the aarch64 cross sysroot.  The test checks:

  * power sequencing writes "1" on start and "0" on SIGTERM,
  * raw 8N1 termios setup succeeds on a tty-like device,
  * valid NMEA sentences are forwarded to the PTY and to the Unix socket,
  * binary (non-NMEA) bytes are dropped and counted, not forwarded,
  * the PTY is exposed at <rundir>/tty and the socket at <rundir>/<name>.
"""
import os
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "j7elte-gpsd")
SYSROOT = "/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc"

NMEA = [
    b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",
    b"$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",
]


def read_until(fd, marker, timeout=10.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            buf += os.read(fd, 65536)
            if marker in buf:
                return buf
    return buf


def main():
    tmp = tempfile.mkdtemp(prefix="gpsd-test-")
    rundir = os.path.join(tmp, "run")
    gpio = os.path.join(tmp, "GPS_PWR_EN")
    open(gpio, "w").write("0")

    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)

    cmd = [
        "qemu-aarch64", "-L", SYSROOT, BIN,
        "--device", slave_name,
        "--baud", "921600",
        "--gpio", gpio,
        "--power-delay", "10",
        "--rundir", rundir,
        "--socket", "test.sock",
        "--verbose",
    ]
    print("exec:", " ".join(cmd))
    log = open(os.path.join(tmp, "daemon.log"), "w+b")
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)

    time.sleep(1.0)
    if open(gpio).read().strip() != "1":
        print("FAIL: power GPIO not asserted")
        p.kill()
        return 1

    # Feed one good sentence, a binary blob, then another good sentence.
    os.write(master, NMEA[0])
    os.write(master, bytes.fromhex("aa4412") + b"\x01\x02\x03\xff\xfe")
    os.write(master, NMEA[1])

    link = os.path.join(rundir, "tty")
    if not os.path.islink(link):
        print("FAIL: %s missing" % link)
        p.kill()
        return 1
    pty_consumer = os.open(link, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    got = read_until(pty_consumer, NMEA[1][:12], 5.0)

    import socket
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(os.path.join(rundir, "test.sock"))
    os.write(master, NMEA[0])
    sock_got = b""
    try:
        sock_got = s.recv(4096)
    except Exception:
        pass

    p.send_signal(signal.SIGTERM)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()

    gpio_after = open(gpio).read().strip()
    log.flush()
    log.seek(0)
    daemon_log = log.read().decode(errors="replace")

    ok = True
    def check(name, cond):
        nonlocal ok
        print(("PASS: " if cond else "FAIL: ") + name)
        ok = ok and cond

    check("power asserted on start", True)
    check("first NMEA sentence reached PTY", NMEA[0].split(b",")[0] in got)
    check("second NMEA sentence reached PTY", NMEA[1].split(b",")[0] in got)
    check("binary bytes not forwarded to PTY", b"\xaa\x44\x12" not in got)
    check("NMEA reached Unix socket", sock_got.startswith(b"$"))
    check("power deasserted on exit", gpio_after == "0")
    check("stats accounted for dropped binary bytes",
          "non-NMEA bytes discarded" in daemon_log)
    check("stable PTY link reported", "NMEA PTY ready" in daemon_log)

    if not ok:
        print("---- daemon log ----")
        print(daemon_log)
    else:
        print("all checks passed; log tail:")
        print("\n".join(daemon_log.strip().splitlines()[-6:]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
