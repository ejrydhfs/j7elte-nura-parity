#!/usr/bin/env python3
"""Static xref/PLT analysis for stock Android gpsd (ARM, Thumb-2).

Usage:
  python3 xrefs.py strings 0xADDR [0xADDR...]   # literal-pool xrefs to addresses
  python3 xrefs.py calls   SYMBOL [SYMBOL...]   # callers of imported funcs
  python3 xrefs.py func    0xADDR [0xADDR...]   # disassemble function at addr
"""
import struct, sys
from capstone import *
from capstone.arm import ARM_INS_BL, ARM_INS_BLX, ARM_OP_REG, ARM_OP_MEM, ARM_REG_PC

BIN = '/tmp/stock_gpsd'
f = open(BIN, 'rb').read()

TEXT_OFF, TEXT_ADDR, TEXT_SIZE = 0x18990, 0x18990, 0x2c510e
PLT_OFF, PLT_ADDR, PLT_SIZE = 0x17a10, 0x17a10, 0xf80
RELPLT_OFF, RELPLT_SIZE = 0x16fc8, 0xa48
DYNSYM_OFF, DYNSYM_SIZE = 0x1a0, 0x1c40
DYNSTR_OFF = 0x1de0

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True
md.skipdata = True


def dynstr(n):
    e = f.index(b'\0', DYNSTR_OFF + n)
    return f[DYNSTR_OFF + n:e].decode()


syms = []
for i in range(DYNSYM_OFF, DYNSYM_OFF + DYNSYM_SIZE, 16):
    name, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', f, i)
    syms.append(dynstr(name))


def build_plt():
    m = {}
    for k, i in enumerate(range(RELPLT_OFF, RELPLT_OFF + RELPLT_SIZE, 8)):
        stub = 0x17a24 + 12 * k
        m.setdefault(syms[struct.unpack_from('<II', f, i)[1] >> 8], []).append(stub)
    return m


PLT = build_plt()
STUB2NAME = {}
for n, ss in PLT.items():
    for s in ss:
        STUB2NAME[s] = n

# word map for literal pools (only inside .text)
WORDS = {}
for a in range(TEXT_ADDR, TEXT_ADDR + TEXT_SIZE - 4, 4):
    WORDS[a] = struct.unpack_from('<I', f, a)[0]


def thumb_lit_addr(insn):
    """Return literal address for ldr rd,[pc,#imm] (Thumb) else None."""
    ops = insn.operands
    if len(ops) == 2 and ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_MEM:
        mem = ops[1].mem
        if mem.base == ARM_REG_PC and mem.index == 0:
            base = (insn.address + 4) & ~3
            return base + mem.disp
    return None


def sweep():
    calls = {}      # target -> [(addr, name)]
    litrefs = {}    # value -> [addr...]
    for insn in md.disasm(f[TEXT_OFF:TEXT_OFF + TEXT_SIZE], TEXT_ADDR):
        if insn.id == 0:
            continue
        if insn.id in (ARM_INS_BL, ARM_INS_BLX):
            tgt = insn.operands[0].imm if insn.operands else None
            if tgt is not None:
                calls.setdefault(tgt & ~1, []).append(insn.address)
        la = thumb_lit_addr(insn)
        if la is not None and la in WORDS:
            litrefs.setdefault(WORDS[la], []).append((insn.address, la))
    return calls, litrefs


CALLS, LITREFS = sweep()


def cmd_strings(addrs):
    for a in addrs:
        print('== string addr', hex(a), repr(f[a:f.index(b'\0', a)][:100]))
        hits = LITREFS.get(a, [])
        for i, la in hits:
            print('   xref code', hex(i), 'literal', hex(la))


def cmd_calls(names):
    for n in names:
        stubs = PLT.get(n, [])
        print('==', n, 'plt', [hex(s) for s in stubs])
        for s in stubs:
            for c in CALLS.get(s, []):
                print('   caller', hex(c))


def find_func_start(addr):
    # walk back over 4-byte aligned push/pac sequences is unreliable; use exidx
    return addr & ~1


def cmd_func(addrs):
    for a in addrs:
        a &= ~1
        print('==== func', hex(a))
        n = 0
        for insn in md.disasm(f[a:a + 0x400], a):
            print(hex(insn.address), insn.mnemonic, insn.op_str)
            n += 1
            if n > 220:
                print('...')
                break


if __name__ == '__main__':
    cmd = sys.argv[1]
    args = sys.argv[2:]
    if cmd == 'strings':
        cmd_strings([int(x, 0) for x in args])
    elif cmd == 'calls':
        cmd_calls(args)
    elif cmd == 'func':
        cmd_func([int(x, 0) for x in args])
