#!/bin/bash
# dis-kbase.sh - disassemble a VA range of the stock raw arm64 Image.
#
# Usage:
#   dis-kbase.sh <kernel.bin> <start-va-hex> [stop-va-hex]
#   dis-kbase.sh <kernel.bin> --sym <name> [<kallsyms.txt>]
#   dis-kbase.sh <kernel.bin> --list <name-regex> [<kallsyms.txt>]
#
# The Image base is 0xffffffc000205000 (= kallsyms _text); objdump runs on the
# raw binary with --adjust-vma=0xffffffc000205000 so the printed addresses are
# real kernel VAs.  (Verify: "kbase_gpu_dump_slots:" is referenced at
# 0xffffffc000b9409b and sits at file offset 0x98f09b.)
#
# Examples:
#   ./dis-kbase.sh kernel.bin --sym kbase_job_hw_submit
#   ./dis-kbase.sh kernel.bin --sym kbase_pm_enable_interrupts
#   ./dis-kbase.sh kernel.bin 0xffffffc0004eff5c 0xffffffc0004f010c

set -eu
BIN=${1:?usage: $0 <kernel.bin> <start-va> [stop-va] | --sym <name> [kallsyms]}
shift

exec python3 - "$BIN" "$@" <<'PY'
import os, re, subprocess, sys

BASE = 0xFFFFFFC000205000
bin_path = sys.argv[1]
args = sys.argv[2:]

def symbols(path):
    out = {}
    for line in open(path):
        p = line.split()
        if len(p) >= 3:
            out.setdefault(p[2], int(p[0], 16))
    return out

if args and args[0] in ("--sym", "--list"):
    mode, pat = args[0], args[1]
    sym_path = args[2] if len(args) > 2 else "kallsyms.txt"
    syms = symbols(sym_path)
    if mode == "--list":
        for name in sorted(syms):
            if re.search(pat, name):
                print(f"{syms[name]:#018x} {name}")
        raise SystemExit(0)
    if pat not in syms:
        sys.exit(f"symbol {pat} not in {sym_path}")
    start = syms[pat]
    higher = [a for a in syms.values() if a > start]
    stop = min(higher) if higher else start + 0x400
else:
    start = int(args[0], 16)
    stop = int(args[1], 16) if len(args) > 1 else start + 0x400

if stop <= start:
    sys.exit(f"bad range {start:#x}..{stop:#x}")
subprocess.run([
    "aarch64-linux-gnu-objdump", "-D", "-b", "binary", "-m", "aarch64",
    f"--adjust-vma={BASE:#x}", f"--start-address={start:#x}",
    f"--stop-address={stop:#x}", bin_path,
], check=False)
PY
