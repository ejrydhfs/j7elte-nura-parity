import subprocess as sp, re
blob='parity/userspace/gpu/blob/libGLES_mali.so'
od='toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/bin/objdump'
data=open(blob,'rb').read()
targets={'dev_mali':0x12d8900,'ddk_range':0x12d889e,'mali_t720':0x12d89b4,
         'gralloc_invalid':0x12d85ac,'base_kbase_assert':0x12d8a00,
         'glsl310':0x12f2e00,'langver':0x12e43fc,'cmar_assert':0x12da47c,
         'not_compatible':0x12d8934,'gles_src_internal':0x132f288,
         'gles_shader_binary':0x132f2e8,'gles_compile_shader':0x132f328,
         'gles_shaders_get':0x1420328,'compiler_frontend_fail':0x142fdc0,
         'compiler_middle_fail':0x142fe08,'support_mali':0x12dda38,'shader_binary_check':0x12f4704}
tmap={v:k for k,v in targets.items()}
def word(v):
    i=v-0x9e000
    if i<0 or i+4>len(data): return None
    return int.from_bytes(data[i:i+4],'little')
p=sp.Popen([od,'-d','--no-show-raw-insn',blob],stdout=sp.PIPE,text=True,errors='replace')
vals={}
label='?'
hits=[]
def check(a,val,how,ins):
    if val is not None and val in tmap:
        hits.append((tmap[val],label,a,val,how,ins))
for line in p.stdout:
    m=re.match(r'^([0-9a-f]{8}) <([^>]+)>:',line)
    if m: label=m.group(2); vals={}; continue
    mm=re.match(r'^\s*([0-9a-f]+):\t(.*)$',line)
    if not mm: continue
    a=int(mm.group(1),16); ins=mm.group(2).strip()
    lm=re.search(r'ldr\s+(r\d+), \[pc, #-?\d+\]\s*; ([0-9a-f]+)',ins)
    if lm:
        la=int(lm.group(2),16)
        vals[lm.group(1)]=('lit',la,word(la))
        continue
    ai=re.match(r'add\s+(r\d+), pc, #(\d+)$',ins)
    if ai:
        v=(a+8+int(ai.group(2)))&0xffffffff
        vals[ai.group(1)]=('abs',v,None)
        check(a,v,'addpc_imm',ins)
        continue
    am=re.match(r'add\s+(r\d+), pc, (r\d+)$',ins)
    if am:
        s=vals.get(am.group(2))
        if s and s[0]=='lit' and s[2] is not None:
            v=(a+8+s[2])&0xffffffff
            vals[am.group(1)]=('abs',v,None)
            check(a,v,'addpc',ins)
        continue
    am2=re.match(r'add(?:s)?\s+(r\d+), (r\d+), #(\d+)$',ins)
    if am2:
        s=vals.get(am2.group(2))
        if s and s[0]=='abs':
            v=(s[1]+int(am2.group(3)))&0xffffffff
            vals[am2.group(1)]=('abs',v,None)
            check(a,v,'addimm',ins)
        continue
    mmv=re.match(r'mov(?:s)?\s+(r\d+), (r\d+)$',ins)
    if mmv:
        s=vals.get(mmv.group(2))
        if s: vals[mmv.group(1)]=s
        continue
p.wait()
print('hits',len(hits))
for h in hits:
    print('%s %-45s %08x val=%x via=%s  %s'%(h[0],h[1][:45],h[2],h[3],h[4],h[5][:50]))
