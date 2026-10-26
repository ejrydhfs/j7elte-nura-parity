import subprocess as sp, re, sys
blob='parity/userspace/gpu/blob/libGLES_mali.so'
od='toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/bin/objdump'
data=open(blob,'rb').read()
targets={'dev_mali':0x12d8900,'ddk_range':0x12d889e,'mali_t720':0x12d89b4,
         'gralloc_invalid':0x12d85ac,'base_kbase_assert':0x12d8a00,
         'glsl310':0x12f2e00,'langver':0x12e43fc,'cmar_assert':0x12da47c,
         'not_compatible':0x12d8934}
tmap={v:k for k,v in targets.items()}
def word(v):
    i=v-0x9e000
    if i<0 or i+4>len(data): return None
    return int.from_bytes(data[i:i+4],'little')
p=sp.Popen([od,'-d','--no-show-raw-insn',blob],stdout=sp.PIPE,text=True,errors='replace')
reg={}
label='?'
hits=[]
for line in p.stdout:
    m=re.match(r'^([0-9a-f]{8}) <([^>]+)>:',line)
    if m:
        label=m.group(2); reg={}; continue
    mm=re.match(r'^\s*([0-9a-f]+):\t(.*)$',line)
    if not mm: continue
    a=int(mm.group(1),16); ins=mm.group(2)
    lm=re.search(r'ldr\s+(r\d+), \[pc, #-?\d+\]\s*; ([0-9a-f]+)',ins)
    if lm:
        reg[lm.group(1)]=int(lm.group(2),16); continue
    am=re.search(r'add\s+(r\d+), pc, (r\d+)',ins)
    if am and am.group(2) in reg:
        W=word(reg[am.group(2)])
        if W is not None:
            tgt=(a+8+W)&0xffffffff
            for delta in (0,4,8):
                t2=(tgt+delta)&0xffffffff
                if t2 in tmap:
                    hits.append((tmap[t2],label,a,reg[am.group(2)],W,t2,ins.strip()))
p.wait()
print('hits',len(hits))
for h in hits:
    print('%s %-45s %08x lit=%x W=%x tgt=%x  %s'%(h[0],h[1][:45],h[2],h[3],h[4],h[5],h[6][:60]))
