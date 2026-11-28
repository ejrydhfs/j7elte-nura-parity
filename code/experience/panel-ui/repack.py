import struct, sys, hashlib

def parse(d):
    magic = d[:8]
    assert magic == b'ANDROID!', magic
    vals = struct.unpack_from('<10I', d, 8)
    ks,kaddr,rs,raddr,ss,saddr,tags,page,dtsz,unused = vals
    def al(n): return (n + page - 1)//page*page
    ko = page
    ro = ko + al(ks)
    so = ro + al(rs)
    do = so + al(ss)
    to = do + al(dtsz)
    return dict(vals=vals, ks=ks, rs=rs, ss=ss, dtsz=dtsz, page=page,
                ko=ko, ro=ro, so=so, do=do, to=to, al=al)

def repack(src, new_kernel_bytes, out):
    d = open(src,'rb').read()
    p = parse(d)
    page = p['page']; al = p['al']
    hdr = bytearray(d[:page])
    ks, rs, ss, dtsz = len(new_kernel_bytes), p['rs'], p['ss'], p['dtsz']
    struct.pack_into('<I', hdr, 8, ks)
    ramdisk = d[p['ro']:p['ro']+p['rs']]
    second  = d[p['so']:p['so']+p['ss']]
    dt      = d[p['do']:p['do']+p['dtsz']]
    tail    = d[p['to']:]
    body = bytearray()
    body += new_kernel_bytes
    body += b'\0' * (al(ks) - ks)
    body += ramdisk + b'\0' * (al(rs) - rs)
    body += second  + b'\0' * (al(ss) - ss)
    body += dt      + b'\0' * (al(dtsz) - dtsz)
    body += tail
    img = bytes(hdr) + bytes(body)
    open(out,'wb').write(img)
    return img

if __name__ == '__main__':
    src, kpath, out = sys.argv[1], sys.argv[2], sys.argv[3]
    kb = open(kpath,'rb').read() if kpath != '-' else None
    if kb is None:
        d = open(src,'rb').read(); p = parse(d); kb = d[p['ko']:p['ko']+p['ks']]
    img = repack(src, kb, out)
    print('wrote', out, len(img), 'bytes', 'md5', hashlib.md5(img).hexdigest())
