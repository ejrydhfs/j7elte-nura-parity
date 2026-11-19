#!/usr/bin/env python3
"""parse-xwd.py - parse an XWD root capture and report surface statistics.

Usage: parse-xwd.py FILE [--stride N]

Mirrors the evidence shape in docs/reports/gpu-verify.md section 3.1:
  xwd header_size 107 version 7 format 2 depth 24 size 720 x 1280
  bytes_per_line 2880 (4 bytes/pixel)
  sampled pixels 172800 distinct 2065

Prints key=value lines: width height depth bits_per_pixel bytes_per_line
sampled distinct, plus top5 colours (hex,count). Exit 0 on success,
1 on unreadable/bad file, 2 on truncated pixel data.

Only the standard library is used (device has python3, no PIL needed).
"""
import struct
import sys
from collections import Counter

HDR_WORDS = 25
HDR_SIZE = HDR_WORDS * 4


def fail(msg, code=1):
    sys.stderr.write("parse-xwd: %s\n" % msg)
    return code


def main(argv):
    path = None
    stride = 4
    i = 0
    while i < len(argv):
        if argv[i] == "--stride" and i + 1 < len(argv):
            stride = int(argv[i + 1])
            i += 2
        elif argv[i].startswith("--stride="):
            stride = int(argv[i].split("=", 1)[1])
            i += 1
        elif path is None:
            path = argv[i]
            i += 1
        else:
            return fail("unexpected argument: %s" % argv[i])
    if path is None:
        return fail("usage: parse-xwd.py FILE [--stride N]")

    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        return fail("%s: %s" % (path, e))

    if len(raw) < HDR_SIZE:
        return fail("%s: too short for XWD header (%d bytes)" % (path, len(raw)))

    hdr = struct.unpack(">25I", raw[:HDR_SIZE])
    (header_size, file_version, pixmap_format, pixmap_depth,
     pixmap_width, pixmap_height, xoffset, byte_order,
     bitmap_unit, bitmap_bit_order, bitmap_pad, bits_per_pixel,
     bytes_per_line, visual_class, red_mask, green_mask, blue_mask,
     bits_per_rgb, colormap_entries, ncolors, window_width,
     window_height, window_x, window_y, window_bdrwidth) = hdr

    if pixmap_format not in (1, 2):
        return fail("%s: unexpected pixmap_format %d" % (path, pixmap_format))
    if pixmap_width <= 0 or pixmap_height <= 0 or bytes_per_line <= 0:
        return fail("%s: insane geometry %dx%d bpl=%d"
                    % (path, pixmap_width, pixmap_height, bytes_per_line))
    if bits_per_pixel not in (1, 8, 16, 24, 32):
        return fail("%s: unsupported bits_per_pixel %d" % (path, bits_per_pixel))

    bpp = (bits_per_pixel + 7) // 8
    if bits_per_pixel == 24:
        # Depth-24 ZPixmap data is stored 32-bit padded in XWD dumps
        # (matches the device capture: depth 24, 4 bytes/pixel).
        bpp = 4

    name_len = max(header_size - HDR_SIZE, 0)
    pix_start = HDR_SIZE + ((name_len + 3) // 4) * 4 + ncolors * 12
    need = pix_start + pixmap_height * bytes_per_line
    if len(raw) < need:
        sys.stderr.write("parse-xwd: %s: truncated pixel data (%d < %d)\n"
                         % (path, len(raw), need))
        return 2

    # Auto-widen the stride so huge captures stay under ~300k samples.
    total = pixmap_width * pixmap_height
    while total // stride > 300000:
        stride += 1

    seen = set()
    top = Counter()
    sampled = 0
    for y in range(pixmap_height):
        base = pix_start + y * bytes_per_line
        row = raw[base:base + bytes_per_line]
        for x in range(0, pixmap_width, stride):
            off = x * bpp
            px = row[off:off + bpp]
            if len(px) < bpp:
                break
            seen.add(px)
            top[px] += 1
            sampled += 1

    print("width=%d" % pixmap_width)
    print("height=%d" % pixmap_height)
    print("depth=%d" % pixmap_depth)
    print("bits_per_pixel=%d" % bits_per_pixel)
    print("bytes_per_line=%d" % bytes_per_line)
    print("sampled=%d" % sampled)
    print("distinct=%d" % len(seen))
    top5 = ",".join("%s:%d" % (px.hex(), c) for px, c in top.most_common(5))
    print("top5=%s" % top5)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
