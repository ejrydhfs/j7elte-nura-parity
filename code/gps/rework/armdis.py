import capstone as cs, struct, sys
from capstone.arm import ARM_REG_PC, ARM_OP_MEM
d=open('/tmp/stock_gpsd','rb').read()
TEXT_OFF=0x18990; TEXT_VA=0x18990; TEXT_SZ=0x2c510e
code=d[TEXT_OFF:TEXT_OFF+TEXT_SZ]
md=cs.Cs(cs.CS_ARCH_ARM,cs.CS_MODE_THUMB); md.detail=False; md.skipdata=True
def show(addr,count=80):
    off=addr-TEXT_VA
    out=0
    for ins in md.disasm(code[off:off+8*count+64], addr):
        mark=''
        if 0x2f0570<= (0) : pass
        print(f'{ins.address:08x}: {ins.mnemonic:<10s} {ins.op_str}')
        out+=1
        if out>=count: break
if __name__=='__main__':
    a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 60
    show(a,n)
