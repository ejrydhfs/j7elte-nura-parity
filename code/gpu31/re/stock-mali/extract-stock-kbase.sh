#!/bin/bash
# extract-stock-kbase.sh - reproduce the stock Mali kbase reference used by
# docs/reports/mali-kbase-stock-reference.md
#
# Usage:  extract-stock-kbase.sh <stock-firmware.zip> [workdir]
#
# Read-only on the archive: the zip is only read, never modified.  Everything
# is written under <workdir> (default: $PWD/stock-mali-re).
#
# Toolchain needed:
#   unzip, tar, python3
#   kallsyms-finder   (pip install vmlinux-to-elf; binary is in the venv bin/)
#   aarch64-linux-gnu-objdump (binutils-aarch64-linux-gnu)
#
# What it does
#   1. streams AP_*.tar.md5 out of the odin zip and pulls boot.img
#   2. parses the Android v0 boot header and carves the kernel out of page 1
#   3. asserts the result is an *uncompressed* arm64 Image ("ARMd" at 0x38)
#   4. recovers kallsyms from the stripped Image (Samsung ships no System.map)
#   5. prints the kbase symbols the report cites
#
# The image base: the arm64 Image header's text_offset is 0x205000 and the
# stock kernel lives at 0xffffffc000000000 + 0x205000, so
#     file offset = VA - 0xffffffc000205000
# (verified independently: kallsyms _text == 0xffffffc000205000 and the
#  "kbase_gpu_dump_slots:" format string the code references at
#  0xffffffc000b9409b is at file offset 0x98f09b).

set -eu

ZIP=${1:?usage: $0 <stock-firmware.zip> [workdir]}
WORK=${2:-$PWD/stock-mali-re}
KALLSYMS_FINDER=${KALLSYMS_FINDER:-kallsyms-finder}
BASE=0xffffffc000205000

mkdir -p "$WORK"
cd "$WORK"

AP=$(unzip -Z1 "$ZIP" | grep -m1 '^AP_.*\.tar\.md5$') || {
	echo "no AP_*.tar.md5 in $ZIP" >&2; exit 1; }
echo "[1/5] extracting $AP (3.2 GiB stream)"
unzip -p "$ZIP" "$AP" > AP.tar.md5

echo "[2/5] pulling boot.img"
tar -xf AP.tar.md5 boot.img

echo "[3/5] carving kernel from boot.img"
python3 - <<'PY'
import struct
d = open('boot.img', 'rb').read()
assert d[:8] == b'ANDROID!', 'not an Android boot image'
ksize = struct.unpack_from('<I', d, 0x08)[0]
page  = struct.unpack_from('<I', d, 0x24)[0]
kern  = d[page:page + ksize]
open('kernel.bin', 'wb').write(kern)
print(f'      page={page} kernel_size={ksize} first4={kern[:4].hex()}')
if kern[:2] == b'\x1f\x8b':
    raise SystemExit('kernel is gzip - decompress before kallsyms/objdump')
if kern[:4] == b'\x02\x21\x4c\x18':
    raise SystemExit('kernel is lz4 - decompress before kallsyms/objdump')
assert kern[0x38:0x3c] == b'ARMd', 'not a raw arm64 Image (offset 0x38 != ARMd)'
tso = struct.unpack_from('<Q', kern, 0x08)[0]
print(f'      raw arm64 Image, text_offset=0x{tso:x}')
PY

echo "[4/5] recovering kallsyms (stripped Image)"
"$KALLSYMS_FINDER" kernel.bin > kallsyms.txt
head -1 kallsyms.txt

echo "[5/5] kbase symbols cited by the report"
grep -nE ' (kbase_job_hw_submit|kbase_job_done|kbase_job_irq_handler|kbase_gpu_interrupt|kbase_reg_write|kbase_reg_read|kbase_exception_name|kbase_pm_enable_interrupts|kbase_pm_disable_interrupts|kbase_pm_init_hw|kbase_pm_clock_on|kbase_gpu_slot_update|kbase_mmu_hw_configure|kbase_mmu_interrupt|kbase_mmu_interrupt_process|kbase_mmu_report_fault_and_kill|kbase_pm_request_cores|kbase_pm_register_inuse_cores|kbase_gpu_dump_slots)$' kallsyms.txt

cat <<EOF

done.  Workdir: $WORK
  kernel.bin    raw arm64 Image, VA->file offset subtract $BASE
  kallsyms.txt  recovered symbols
  boot.img      /  AP.tar.md5 (3.3 GiB, safe to delete)

Disassemble a symbol:
  ./dis-kbase.sh $WORK/kernel.bin \$((0x\$(awk '/ kbase_job_hw_submit$/{print \$1}' $WORK/kallsyms.txt)))
EOF
