import fcntl, mmap, os, struct, sys

fd = os.open('/dev/dri/renderD128', os.O_RDWR)
print('fd', fd)

# struct drm_mode_create_dumb: u32 height,width,bpp,flags,handle,pitch; u64 size
b = bytearray(32)
struct.pack_into('<6I', b, 0, 1280, 768, 32, 0, 0, 0)
try:
    fcntl.ioctl(fd, 0xC02064B2, b, True)
except OSError as e:
    print('CREATE_DUMB failed', e)
    sys.exit(1)
h, w, bpp, flags, handle, pitch = struct.unpack_from('<6I', b, 0)
size = struct.unpack_from('<Q', b, 24)[0]
print('handle', handle, 'pitch', pitch, 'size', size)

# struct drm_mode_map_dumb: u32 handle; u32 pad; u64 offset
mb = bytearray(16)
struct.pack_into('<II', mb, 0, handle, 0)
try:
    fcntl.ioctl(fd, 0xC01064B3, mb, True)
except OSError as e:
    print('MAP_DUMB failed', e)
    sys.exit(1)
off = struct.unpack_from('<Q', mb, 8)[0]
print('offset', hex(off))

m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
print('mmap ok, len', len(m))
chunk = b'\x11' * (1 << 20)
done = 0
while done < size:
    n = min(len(chunk), size - done)
    m[done:done + n] = chunk[:n]
    done += n
print('write ok, readback', m[0], m[size - 1])
m.flush()
print('flush ok')
