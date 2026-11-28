import struct, sys, hashlib
PART = 33554432  # 32 MiB partition

def build(src, kernel_bytes, out):
    d = open(src, 'rb').read()
    (ks,kaddr,rs,raddr,ss,saddr,tags,page,dtsz,unused) = struct.unpack_from('<10I', d, 8)
    al = lambda n: (n + page - 1)//page*page
    ko = page; ro = ko+al(ks); so = ro+al(rs); do = so+al(ss); to = do+al(dtsz)
    hdr = bytearray(d[:page])
    nks = len(kernel_bytes)
    struct.pack_into('<I', hdr, 8, nks)
    ramdisk = d[ro:ro+rs]
    second  = d[so:so+ss]
    dt      = d[do:do+dtsz]
    body = bytearray()
    body += kernel_bytes + b'\0'*(al(nks)-nks)
    body += ramdisk      + b'\0'*(al(rs)-rs)
    body += second       + b'\0'*(al(ss)-ss)
    body += dt           + b'\0'*(al(dtsz)-dtsz)
    img = bytearray(hdr) + body
    if len(img) > PART:
        raise SystemExit('image %d exceeds partition %d' % (len(img), PART))
    img += b'\0' * (PART - len(img))     # zero-fill, no stale tail
    open(out, 'wb').write(img)
    return bytes(img), dict(ks=nks, rs=rs, dtsz=dtsz, page=page, ko=ko, ro=ro, do=do,
                            nro=page+al(nks), ndo=page+al(nks)+al(rs))

if __name__ == '__main__':
    src, kpath, out = sys.argv[1], sys.argv[2], sys.argv[3]
    img, info = build(src, open(kpath,'rb').read(), out)
    print('wrote', out, len(img), 'md5', hashlib.md5(img).hexdigest())
    print('kernel %d ramdisk %d dt %d' % (info['ks'], info['rs'], info['dtsz']))
