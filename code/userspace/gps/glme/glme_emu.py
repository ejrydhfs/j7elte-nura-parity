#!/usr/bin/env python3
# glme_emu.py - BCM4752 BRI+GLME chip emulator for stock-gpsd trace capture.
# Lives in the repo so the rig is reproducible without the device.
# BRI phase: proven answers (bank tracking, ESW CRC values from hardware,
# sync echo, register/item reads) up to the 0x55aa switch.
# GLME phase: send the REAL chip banner bytes, then answer NOTHING
# host-ward; log every post-switch byte with a monotonic timestamp:
# each read as RX-RAW (exact length + hex, no truncation) and each
# e0..e0 01 frame as RX-GLME. Frames appended to OUT as
# "FRAME <n> <len>: <hex>". Never touches /dev/ttySAC0.
import os, pty, select, struct, sys, time

LOG = sys.argv[1] if len(sys.argv) > 1 else 'emu.log'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'host-frames.txt'
DUR = float(sys.argv[3]) if len(sys.argv) > 3 else 90
# Real BCM4752 banner (golden.log glme-rx, 17 bytes, [HW] 2026-11-19).
BANNER = bytes.fromhex('e0000000071002017206300200055fe001')

os.makedirs(os.path.dirname(os.path.abspath(LOG)), exist_ok=True)
os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
logf = open(LOG, 'w', buffering=1)
outf = open(OUT, 'w', buffering=1)
mfd, sfd = pty.openpty()
slave = os.ttyname(sfd)
t0 = time.time()

def log(s):
    logf.write('%8.3f %s\n' % (time.time() - t0, s))

log('PTY %s dur=%s' % (slave, DUR))
print(slave, flush=True)
bank = 0
glme = False
buf = b''
gbuf = b''
nframes = 0

def reply(b):
    log('TX ' + b.hex())
    os.write(mfd, b)

def emit_frame(f):
    global nframes
    nframes += 1
    log('RX-GLME(%d) %s' % (len(f), f.hex()))
    outf.write('FRAME %d %d: %s\n' % (nframes, len(f), f.hex()))
    outf.flush()

def handle_bri(f):
    global bank, glme
    log('RX ' + f.hex())
    if len(f) < 5:
        return
    if f[0] == 0xfe and f[3] == 0x61 and len(f) > 5 and f[4] == 0x16:
        bank = f[5]
        log('   bank select -> %d' % bank)
        return
    if f[0] == 0xfe and f[3] == 0x61 and len(f) > 4 and f[4] == 0x17:
        return
    if f[0] == 0xfe and f[3] == 0x21 and len(f) > 4 and f[4] == 0x18:
        a = struct.unpack('<H', f[5:7])[0]
        ln = struct.unpack('<H', f[7:9])[0]
        log('   CRC req addr=%04x len=%d' % (a, ln))
        acks = {(0x7000, 4060): '0c7307f1', (0x7fec, 4): '750000ec',
                (0x8000, 4096): 'd2ae0981', (0x9000, 2681): '95ed050b',
                (0x7fd2, 4): '00000000'}
        v = acks.get((a, ln), '00000000')
        time.sleep(0.03)
        reply(bytes.fromhex('fe00fd2f15' + v + 'fc'))
        return
    if f[0] == 0xfe and f[3] == 0x21 and len(f) > 4 and f[4] == 0x00:
        reply(f)
        return
    if f[0] == 0xfe and f[3] == 0x00 and len(f) >= 9 and f[5] == 0x55 and f[6] == 0xaa:
        log('   *** GLME SWITCH ***')
        time.sleep(0.05)
        reply(BANNER)
        glme = True
        log('   banner sent (%d bytes), silent from here' % len(BANNER))
        return
    if f[0] == 0xfe and f[3] == 0x00:
        return
    if f[0] == 0xff and f[3] == 0x00:
        reg = f[4]
        val = 0x30020005 if reg == 0 else 0
        reply(bytes([0xfe, 0, 0xfd, 0]) + bytes([reg]) + struct.pack('<I', val) + b'\xfc')
        return
    if f[0] == 0xff and f[3] == 0x1f:
        it = f[4]
        val = {0x94: bank, 0x95: 0, 0x1b: 7, 0x98: 1}.get(it, 0)
        reply(bytes([0xfe, 0, 0xfd, 0x1f, it]) + struct.pack('<I', val) + b'\xfc')
        return
    log('   (no reply)')

def bri_len(b):
    if len(b) < 5:
        return None
    if b[0] not in (0xfe, 0xff):
        return 1
    if b[0] == 0xfe:
        if len(b) > 4 and b[3] == 0x61 and b[4] == 0x17:
            return 9 + 4 * b[1] + 1
        return 10
    return 6

def handle_glme_bytes(d):
    global gbuf
    log('RX-RAW(%d) %s' % (len(d), d.hex()))
    gbuf += d
    while True:
        i = 0
        while i < len(gbuf) and gbuf[i] != 0xe0:
            i += 1
        if i:
            log('RESYNC-GLME drop %s' % gbuf[:i].hex())
            gbuf = gbuf[i:]
            continue
        j = -1
        for k in range(1, len(gbuf) - 1):
            if gbuf[k] == 0xe0 and gbuf[k + 1] == 0x01:
                j = k + 2
                break
        if j < 0:
            break
        f = gbuf[:j]
        gbuf = gbuf[j:]
        emit_frame(f)

end = time.time() + DUR
while time.time() < end:
    r, _, _ = select.select([mfd], [], [], 0.2)
    if not r:
        continue
    try:
        d = os.read(mfd, 65536)
    except OSError:
        break
    if not d:
        continue
    if not glme:
        buf += d
        while buf:
            L = bri_len(buf)
            if L is None:
                break
            if L == 1:
                j = 0
                while j < len(buf) and buf[j] == 0x80:
                    j += 1
                if j:
                    log('RX %s (autobaud 0x80 x%d)' % ('80' * min(j, 32), j))
                    buf = buf[j:]
                    continue
                log('RESYNC drop %02x' % buf[0])
                buf = buf[1:]
                continue
            if len(buf) < L:
                break
            f = buf[:L]
            buf = buf[L:]
            handle_bri(f)
    else:
        handle_glme_bytes(d)
if gbuf:
    log('INCOMPLETE(%d) %s' % (len(gbuf), gbuf.hex()))
log('END glme=%s frames=%d' % (glme, nframes))
