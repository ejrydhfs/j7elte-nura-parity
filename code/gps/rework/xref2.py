import capstone as cs, struct, json, sys
from capstone.arm import ARM_REG_PC, ARM_OP_MEM
d=open('/tmp/stock_gpsd','rb').read()
TEXT_OFF=0x18990; TEXT_VA=0x18990; TEXT_SZ=0x2c510e
code=d[TEXT_OFF:TEXT_OFF+TEXT_SZ]
md=cs.Cs(cs.CS_ARCH_ARM,cs.CS_MODE_THUMB); md.detail=True; md.skipdata=True
pend={}
hits={}
for ins in md.disasm(code, TEXT_VA):
    if ins.id==0: continue
    ops=ins.operands
    m=ins.mnemonic
    if m.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC:
        pool=((ins.address+4)&~3)+ops[1].mem.disp
        if 0<=pool-TEXT_VA<TEXT_SZ-3:
            pend[ops[0].reg]=struct.unpack_from('<I',code,pool-TEXT_VA)[0]
    elif m in ('add','add.w') and len(ops)==2 and ops[1].type==cs.CS_OP_REG and ops[1].reg==ARM_REG_PC:
        r=ops[0].reg
        if r in pend:
            tgt=pend.pop(r)+ins.address+4
            hits.setdefault(tgt,[]).append((ins.address, r))
json.dump({str(k):[a for a,_ in v] for k,v in hits.items()}, open('xref2.json','w'))
if __name__=='__main__':
    for a in sys.argv[1:]:
        t=int(a,16); print(hex(t), [hex(x) for x in hits.get(t,[])][:10])
