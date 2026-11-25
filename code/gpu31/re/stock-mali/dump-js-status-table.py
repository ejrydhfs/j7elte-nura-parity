#!/usr/bin/env python3
"""dump-js-status-table.py - decode the stock kbase JM status/exception table.

kbase_job_done() reads JS<n>_STATUS and logs it through kbase_exception_name():

  ffffffc0004daa74: cmp  w1, #0xd8
  ffffffc0004daa7c: adrp x0, 0xffffffc0009ba000
  ffffffc0004daa80: add  x0, x0, #0x710
  ffffffc0004daa84: ldr  x0, [x0, w1, uxtw #3]     ; char *table[status]

so the table is 0xd9 (217) 8-byte pointers at VA 0xffffffc0009ba710.
The decoded entries 0x00, 0x01, 0x08 are why js_status=0x08 is ACTIVE and not
"no exception".

Usage: dump-js-status-table.py <kernel.bin> [table-va]
"""
import signal
import struct
import sys

IMG_BASE = 0xFFFFFFC000205000
TABLE_VA = 0xFFFFFFC0009BA710
NENT = 0xD9


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    table_va = int(sys.argv[2], 16) if len(sys.argv) > 2 else TABLE_VA
    off = table_va - IMG_BASE
    if off < 0 or off + 8 * NENT > len(data):
        print(f"table VA {table_va:#x} outside image", file=sys.stderr)
        return 1

    def cstr(va: int) -> str:
        o = va - IMG_BASE
        if not (0 <= o < len(data)):
            return "<bad pointer>"
        e = data.find(b"\0", o)
        return data[o:e].decode("latin-1", "replace")

    ptrs = struct.unpack_from("<%dQ" % NENT, data, off)
    names = [cstr(p) for p in ptrs]

    i = 0
    while i < NENT:
        j = i
        while j + 1 < NENT and names[j + 1] == names[i]:
            j += 1
        span = f"{i:#04x}" if i == j else f"{i:#04x}-{j:#04x}"
        print(f"{span} {names[i]}")
        i = j + 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
