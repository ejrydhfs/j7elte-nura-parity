#!/usr/bin/env python3
"""Merge a kconfig fragment into .config (both enable and disable lines).
Usage: ci-merge-frag.py CONFIG FRAGMENT
Same merge semantics as code/gpu31/config/check-matrix.sh.
"""
import re
import sys

cfg_path, frag_path = sys.argv[1], sys.argv[2]
req = []
for line in open(frag_path):
    s = line.strip()
    if re.match(r'# CONFIG_\w+ is not set', s):
        req.append(s)
        continue
    if not s or s.startswith('#'):
        continue
    if re.match(r'CONFIG_\w+=', s):
        req.append(s)
cfg = open(cfg_path).read().splitlines()
out = list(cfg)
for f in req:
    sym = f.split()[1] if f.startswith('#') else f.split('=')[0]
    dis = '# %s is not set' % sym
    have = {l.split('=')[0]: i for i, l in enumerate(out) if l.startswith('CONFIG_')}
    if f.startswith('#'):
        if sym in have:
            out[have[sym]] = f
        elif dis not in out:
            out.append(f)
    elif sym in have:
        out[have[sym]] = f
    elif dis in out:
        out[out.index(dis)] = f
    else:
        out.append(f)
open(cfg_path, 'w').write('\n'.join(out) + '\n')
print('merged %d: %s' % (len(req), ' '.join(
    x.split()[1] if x.startswith('#') else x.split('=')[0] for x in req)))
