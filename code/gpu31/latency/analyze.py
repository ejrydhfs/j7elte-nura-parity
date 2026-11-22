#!/usr/bin/env python3
# Summarise iotrace logs: per-DRM-command latency and per-frame submits.
# Usage: ./analyze.py /tmp/iotrace-clean-1.log [...]
import collections
import statistics
import sys

CMD = {
    0x00: "SUBMIT", 0x01: "WAIT_BO", 0x02: "CREATE_BO", 0x03: "MMAP_BO",
    0x04: "GET_BO_OFFSET", 0xC0: "GET_PARAM", 0xC1: "?c1", 0xC2: "?c2",
    0xC3: "?c3", 0xBF: "?bf", 0x48: "MADVISE", 0x0C: "?0c",
    0x41: "WAIT_BO", 0x40: "SUBMIT",
}


def load(path):
    rows = []
    for line in open(path):
        t0, fd, req, ret, dur = line.split()
        rows.append((int(t0), int(fd), int(req, 16), int(ret), int(dur)))
    return rows


def report(path, frames):
    rows = load(path)
    if not rows:
        print(f"{path}: empty")
        return
    print(f"== {path}: {len(rows)} DRM ioctls, "
          f"wall {(rows[-1][0] - rows[0][0]) / 1e9:.3f}s, "
          f"frames={frames}, "
          f"frame={(rows[-1][0] - rows[0][0]) / 1e9 / frames * 1e3:.3f} ms")
    per = collections.defaultdict(list)
    for r in rows:
        per[r[2] & 0xff].append(r[4])
    for nr, ds in sorted(per.items(), key=lambda kv: -sum(kv[1])):
        print(f"  {CMD.get(nr, hex(nr)):14s} n={len(ds):5d} "
              f"mean={statistics.mean(ds) / 1e6:8.3f} ms "
              f"p50={statistics.median(ds) / 1e6:8.3f} ms "
              f"max={max(ds) / 1e6:8.3f} ms "
              f"sum={sum(ds) / 1e6:8.3f} ms")
    subs = [r for r in rows if (r[2] & 0xff) == 0x40]
    gaps = [subs[i + 1][0] - (subs[i][0] + subs[i][4])
            for i in range(len(subs) - 1)]
    if gaps:
        print(f"  submit-to-submit user gap: mean={statistics.mean(gaps) / 1e6:.3f} ms")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: analyze.py LOG [LOG...]")
    for p in sys.argv[1:]:
        report(p, 300)
