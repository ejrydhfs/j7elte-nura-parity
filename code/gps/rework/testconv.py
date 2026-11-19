import capstone as cs, struct, re
from capstone.arm import ARM_REG_PC, ARM_OP_MEM
d=open('/tmp/stock_gpsd','rb').read()
TEXT_OFF=0x18990; TEXT_VA=0x18990; TEXT_SZ=0x2c510e
code=d[TEXT_OFF:TEXT_OFF+TEXT_SZ]
ro_start, ro_end = 0x2f0570, 0x3bff81
ro=d[ro_start:ro_end]
starts=set()
starts.add(ro_start)
for m in re.finditer(rb'[\x20-\x7e]{4,}', ro):
    starts.add(ro_start+m.start())
print('string starts', len(starts))
md=cs.Cs(cs.CS_ARCH_ARM,cs.CS_MODE_THUMB); md.detail=True; md.skipdata=True
hits_abs=0; hits_rel=0; tot=0
pending={}
n=0
for ins in md.disasm(code, TEXT_VA):
    n+=1
    if ins.id==0: continue
    ops=ins.operands
    m=ins.mnemonic
    if m.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC:
        pool=((ins.address+4)&~3)+ops[1].mem.disp
        if 0<=pool-TEXT_VA<TEXT_SZ-3:
            val=struct.unpack_from('<I',code,pool-TEXT_VA)[0]
            tot+=1
            if val in starts: hits_abs+=1
            pending[ops[0].reg]=(ins.address,val)
    elif m=='add' and len(ops)==2 and ops[1].type==cs.CS_OP_REG and ops[1].reg==ARM_REG_PC and ops[0].type==cs.CS_OP_REG:
        r=ops[0].reg
        if r in pending:
            a,val=pending[r]
            rel=val+ins.address+4
            if rel in starts: hits_rel+=1
            del pending[r]
print('ldr-pc total',tot,'abs hits',hits_abs,'rel hits',hits_rel)
