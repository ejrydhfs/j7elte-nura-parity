#!/usr/bin/env python3
"""Annotated disassembly for stock gpsd (Thumb-2, PC-relative string refs).

The binary links string refs as: ldr rX,[pc,#imm] (delta) ; add rX, pc
so we resolve ldr/add pairs to .rodata strings.
"""
import struct, sys, bisect
from capstone import *
from capstone.arm import *

BIN = '/tmp/stock_gpsd'
f = open(BIN, 'rb').read()
TEXT = (0x18990, 0x2c510e)
# PLT stub -> imported symbol map
import struct as _st
_DYNSYM, _DYNSZ, _DYNSTR = 0x1a0, 0x1c40, 0x1de0
_RELPLT, _RELPLTSZ = 0x16fc8, 0xa48


def _ds(n):
    e = f.index(b'\0', _DYNSTR + n)
    return f[_DYNSTR + n:e].decode()


_syms = [_ds(_st.unpack_from('<I', f, i)[0]) for i in range(_DYNSYM, _DYNSYM + _DYNSZ, 16)]
PLT = {}
for _k, _i in enumerate(range(_RELPLT, _RELPLT + _RELPLTSZ, 8)):
    PLT[0x17a24 + 12 * _k] = _syms[_st.unpack_from('<II', f, _i)[1] >> 8]

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True
md.skipdata = True


def cstr(a):
    if not (0x2f0570 <= a < 0x3bff84):
        return None
    e = f.find(b'\0', a)
    if e < 0 or e - a > 200:
        return None
    try:
        s = f[a:e].decode('utf-8')
    except Exception:
        return None
    if any(ord(c) < 9 for c in s):
        return None
    return s


def lit_addr(insn):
    ops = insn.operands
    if len(ops) == 2 and ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_MEM:
        m = ops[1].mem
        if m.base == ARM_REG_PC and m.index == 0:
            return ((insn.address + 4) & ~3) + m.disp
    return None


def dump(start, size, maxn=400):
    regdelta = {}
    for insn in md.disasm(f[start:start + size], start):
        if insn.id == 0:
            regdelta.clear()
            print(hex(insn.address), '.word', insn.op_str)
            continue
        ann = ''
        la = lit_addr(insn)
        if la is not None and 0 <= la < len(f) - 4:
            val = struct.unpack_from('<I', f, la)[0]
            ops = insn.operands
            rd = ops[0].reg if ops and ops[0].type == ARM_OP_REG else None
            if insn.mnemonic.startswith('ldr') and rd is not None:
                regdelta[rd] = val
                tgt = None
            else:
                tgt = val
            txt = cstr(val)
            ann = '   ; [%#x]=%#x%s' % (la, val, ' "%s"' % txt.replace('\n', '\\n')[:80] if txt else '')
        if insn.mnemonic.startswith('add'):
            o = insn.operands
            if len(o) == 3 and o[2].type == ARM_OP_REG and o[2].reg == ARM_REG_PC:
                src, dst = (o[1], o[0])
            elif len(o) == 2 and o[1].type == ARM_OP_REG and o[1].reg == ARM_REG_PC:
                src, dst = (o[0], o[0])   # add rD, pc  -> rD = pc + rD
            else:
                src = dst = None
            if src is not None and src.type == ARM_OP_REG and dst.type == ARM_OP_REG and src.reg in regdelta:
                d = regdelta[src.reg]
                if d is not None:
                    tgt = insn.address + 4 + d
                    txt = cstr(tgt)
                    ann = '   ; =%#x%s' % (tgt, ' "%s"' % txt.replace('\n', '\\n')[:80] if txt else '')
                regdelta[dst.reg] = None
        if insn.mnemonic in ('bl', 'blx') and insn.operands:
            t = insn.operands[0].imm & ~1
            if t in PLT:
                ann += '   ; -> %s@plt' % PLT[t]
        print(hex(insn.address), insn.mnemonic, insn.op_str + ann)
        maxn -= 1
        if maxn <= 0:
            print('...')
            return


if __name__ == '__main__':
    for a in sys.argv[1:]:
        if '-' in a:
            s, e = a.split('-')
            s, e = int(s, 0), int(e, 0)
            print('==== %#x..%#x' % (s, e))
            dump(s, e - s)
        else:
            s = int(a, 0)
            print('==== %#x +0x200' % s)
            dump(s, 0x200)
