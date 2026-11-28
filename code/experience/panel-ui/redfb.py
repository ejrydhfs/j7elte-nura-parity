import os, struct
w, h = 720, 1280
f = os.open('/dev/fb0', os.O_RDWR)
data = struct.pack('<I', 0x00ff0000) * (w * h)
os.write(f, data)
os.close(f)
print('wrote', len(data))
