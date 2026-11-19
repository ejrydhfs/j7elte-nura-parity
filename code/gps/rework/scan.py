import capstone as cs, struct, json, sys
from capstone.arm import ARM_REG_PC, ARM_OP_MEM, ARM_OP_REG
d=open('/tmp/stock_gpsd','rb').read()
TEXT_OFF=0x18990; TEXT_VA=0x18990; TEXT_SZ=0x2c510e
code=d[TEXT_OFF:TEXT_OFF+TEXT_SZ]
md=cs.Cs(cs.CS_ARCH_ARM,cs.CS_MODE_THUMB); md.detail=True; md.skipdata=True
xref={}   # literal value -> list of insn addresses
litloads={}
n=0
for ins in md.disasm(code, TEXT_VA):
    n+=1
    if ins.id==0: continue
    ops=ins.operands
    if ins.mnemonic.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC and ops[1].mem.index==0:
        pool=((ins.address+4)&~3)+ops[1].mem.disp
        if 0<=pool-TEXT_VA<TEXT_SZ-3:
            val=struct.unpack_from('<I',code,pool-TEXT_VA)[0]
            xref.setdefault(val,[]).append(ins.address)
            litloads[ins.address]=(pool,val)
print('insns',n,'distinct lits',len(xref),file=sys.stderr)
json.dump({'xref':{str(k):v for k,v in xref.items()},'lit':{str(k):v for k,v in litloads.items()}}, open('xrefs.json','w'))
