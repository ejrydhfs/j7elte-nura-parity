import capstone as cs, struct
from capstone.arm import ARM_REG_PC, ARM_OP_MEM
d=open('/tmp/stock_gpsd','rb').read()
TEXT_OFF=0x18990; TEXT_VA=0x18990; TEXT_SZ=0x2c510e
code=d[TEXT_OFF:TEXT_OFF+TEXT_SZ]
md=cs.Cs(cs.CS_ARCH_ARM,cs.CS_MODE_THUMB); md.detail=True; md.skipdata=True
insns=[]
for ins in md.disasm(code, TEXT_VA):
    if ins.id==0:
        insns.append((ins.address,None,None,[])); continue
    insns.append((ins.address, ins.mnemonic, ins.op_str, ins.operands))
# find base register setups then immediate uses
out=[]
i=0
n=len(insns)
while i<n:
    a,m,o,ops=insns[i]
    if m and m.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC:
        pool=((a+4)&~3)+ops[1].mem.disp
        if 0<=pool-TEXT_VA<TEXT_SZ-3:
            val=struct.unpack_from('<I',code,pool-TEXT_VA)[0]
            reg=ops[0].reg
            # look ahead up to 3 for add reg, pc
            for j in range(i+1, min(i+4,n)):
                a2,m2,o2,ops2=insns[j]
                if m2 in ('add','add.w') and len(ops2)==2 and ops2[1].type==cs.CS_OP_REG and ops2[1].reg==ARM_REG_PC and ops2[0].reg==reg:
                    base=val+a2+4
                    if base==0x398754:
                        # collect following immediate uses of reg
                        uses=[]
                        for k in range(j+1, min(j+25,n)):
                            a3,m3,o3,ops3=insns[k]
                            if not m3: continue
                            if reg in [x.reg for x in ops3 if x.type==cs.CS_OP_REG] and '#' in o3:
                                uses.append((hex(a3),m3,o3))
                        out.append((hex(a),uses))
                    break
    i+=1
for ldr,uses in out:
    print('ldr@',ldr)
    for u in uses: print('   ',u)
