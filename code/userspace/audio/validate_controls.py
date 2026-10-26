#!/usr/bin/env python3
"""Cross-check j7elte audio control names against the Nura kernel sources.

Inputs (paths relative to the repository root, override with --root):
  android_kernel_samsung_universal7580-*/sound/soc/codecs/cod3022x.{c,h}
  android_kernel_samsung_universal7580-*/sound/soc/codecs/s2801x.{c,h}
  android_kernel_samsung_universal7580-*/sound/soc/samsung/universal7580-cod3022x.c
  parity/userspace/audio/rom/mixer_paths.xml
  parity/userspace/audio/ucm2/**/*.conf

Checks performed:
  1. every <ctl name="..."> in the ROM mixer_paths.xml exists as an ASoC
     control of the Nura kernel (exact name, including the "S2801 " prefix
     that ASoC applies through snd_soc_codec_conf.name_prefix);
  2. every cset "name='...'" in the UCM files exists as well;
  3. every textual value in the UCM files is a valid enum text of the
     corresponding codec;
  4. numeric values for the scalar controls stay inside the control's
     declared range.

Exit status: 0 if no problem, 1 otherwise.
"""

import argparse
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

CTRL_RE = re.compile(r'SOC_[A-Z0-9_]+\(\s*"([^"]+)"')
DAPM_SINGLE_RE = re.compile(r'SOC_DAPM_SINGLE\(\s*"([^"]+)"')
DAPM_ARRAY_RE = re.compile(r'SND_SOC_DAPM_(SWITCH|MIXER|MUX)\(\s*"([^"]+)"')
DEFINE_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*(?:/\*.*)?$')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def find_kernel(root):
    dirs = glob.glob(os.path.join(root, 'android_kernel_samsung_universal7580-*'))
    dirs = [d for d in dirs if os.path.isdir(os.path.join(d, 'sound/soc'))]
    if not dirs:
        sys.exit('kernel tree not found under %s' % root)
    return dirs[0]


def array_body(text, var):
    """Return the test between `var[] = {` and the matching `};`."""
    m = re.search(r'\b' + re.escape(var) + r'\s*\[\s*\]\s*=\s*\{', text)
    if not m:
        return ''
    depth = 1
    i = m.end()
    start = i
    while i < len(text) and depth:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    return text[start:i - 1]


def extract_defines(text, table):
    for line in text.splitlines():
        m = DEFINE_RE.match(line)
        if m:
            table[m.group(1)] = m.group(2).strip()


def eval_expr(expr, defines, depth=0):
    if depth > 20:
        return None
    expr = expr.strip()
    expr = re.sub(r'\bBIT\(\s*([A-Za-z0-9_]+)\s*\)',
                  lambda m: '(1 << %s)' % resolve(m.group(1), defines, depth + 1),
                  expr)
    expr = re.sub(r'\bMASK\([^)]*\)', '0', expr)

    def repl(m):
        val = resolve(m.group(0), defines, depth + 1)
        return str(val) if val is not None else m.group(0)

    expr = re.sub(r'\b[A-Za-z_][A-Za-z0-9_]*\b', repl, expr)
    expr = expr.replace('/', '//')
    try:
        return int(eval(expr, {'__builtins__': {}}, {}))
    except Exception:
        return None


def resolve(name, defines, depth=0):
    if name.isdigit():
        return int(name)
    if depth > 20 or name not in defines:
        return None
    return eval_expr(defines[name], defines, depth + 1)


def split_args(text):
    """Split a C macro argument list on top-level commas."""
    args, depth, cur = [], 0, []
    for ch in text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        args.append(''.join(cur).strip())
    return args


def macro_calls(text, prefix):
    """Yield (macro suffix, name, args) for SOC_<prefix>(\"name\", ...) calls."""
    pat = re.compile(r'\bSOC_(' + prefix + r')\(\s*"([^"]+)"\s*,')
    for m in pat.finditer(text):
        depth, i = 0, m.start() + m.group(0).index('(')
        j = i
        while j < len(text):
            if text[j] == '(':
                depth += 1
            elif text[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        call = text[i + 1:j]
        # drop the leading "name" argument
        body = call.split(',', 1)[1] if ',' in call else ''
        yield m.group(1), m.group(2), split_args(body)


def parse_enums(text):
    """Return {enum texts variable: [texts]} and {kcontrol name: texts variable}."""
    texts = {}
    for m in re.finditer(r'static\s+const\s+char\s*\*\s*(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};',
                         text, re.S):
        vals = re.findall(r'"([^"]*)"', m.group(2))
        if vals:
            texts[m.group(1)] = vals

    enum_var_texts = {}
    # static const struct soc_enum <var> = SOC_ENUM_SINGLE(... , <texts>)
    for m in re.finditer(r'static\s+const\s+struct\s+soc_enum\s+(\w+)\s*=\s*[^;]*?\((.*?)\)\s*;',
                         text, re.S):
        var, body = m.group(1), m.group(2)
        idents = re.findall(r'\b([A-Za-z_]\w*)\b', body)
        for ident in reversed(idents):
            if ident in texts:
                enum_var_texts[var] = ident
                break
    # SOC_ENUM_SINGLE_DECL(<var>, reg, shift, <texts>)
    for m in re.finditer(r'SOC_ENUM_SINGLE_DECL\(\s*(\w+)\s*,\s*[^,]+,[^,]+,\s*(\w+)\s*\)',
                         text):
        if m.group(2) in texts:
            enum_var_texts[m.group(1)] = m.group(2)

    enum_of_ctrl = {}
    for m in re.finditer(r'SOC_ENUM(?:_EXT)?\(\s*"([^"]+)"\s*,\s*(\w+)', text):
        enum_of_ctrl[m.group(1)] = m.group(2)
    for m in re.finditer(r'SOC_ENUM_SINGLE\(\s*"([^"]+)"\s*,\s*(\w+)', text):
        enum_of_ctrl[m.group(1)] = m.group(2)

    return texts, enum_of_ctrl, enum_var_texts


MAX_INDEX = {
    'SINGLE_TLV': 2,
    'SINGLE_TLV_EXT': 2,
    'SINGLE_RANGE_TLV': 3,
    'DOUBLE_TLV': 3,
    'DOUBLE_R_TLV': 3,
    'DOUBLE_R_EXT_TLV': 3,
    'SINGLE': 2,
    'SINGLE_EXT': 2,
    'SINGLE_RANGE': 2,
}


def parse_scalar_ranges(text, defines):
    """Return {kcontrol name: (min, max)} for numeric controls."""
    ranges = {}
    for suffix, name, args in macro_calls(text, r'\w+'):
        idx = MAX_INDEX.get(suffix)
        if idx is None or len(args) <= idx:
            continue
        mx = eval_expr(args[idx], defines)
        if mx is None:
            continue
        lo = 0
        if suffix == 'SINGLE_RANGE_TLV' and len(args) > 2:
            mn = eval_expr(args[2], defines)
            if mn is not None:
                lo = mn
        ranges[name] = (lo, mx)
    return ranges


def widget_controls(text, prefix=''):
    """Yield full DAPM kcontrol names ("<widget> <control>") from a codec file."""
    for m in DAPM_ARRAY_RE.finditer(text):
        widget = m.group(2)
        depth, i = 0, m.start() + m.group(0).index('(')
        j = i
        while j < len(text):
            if text[j] == '(':
                depth += 1
            elif text[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        call = text[i + 1:j]
        seen = set()
        for arr in re.findall(r'\b([A-Za-z_]\w*)\b', call):
            if arr in seen:
                continue
            seen.add(arr)
            body = array_body(text, arr)
            if 'SOC_DAPM_SINGLE' not in body:
                continue
            for ctl in DAPM_SINGLE_RE.findall(body):
                yield prefix + '%s %s' % (widget, ctl)


def collect_kernel_controls(root):
    kdir = find_kernel(root)
    codec_dir = os.path.join(kdir, 'sound/soc/codecs')
    mach_dir = os.path.join(kdir, 'sound/soc/samsung')

    cod_c = strip_comments(read(os.path.join(codec_dir, 'cod3022x.c')))
    cod_h = strip_comments(read(os.path.join(codec_dir, 'cod3022x.h')))
    s28_c = strip_comments(read(os.path.join(codec_dir, 's2801x.c')))
    s28_h = strip_comments(read(os.path.join(codec_dir, 's2801x.h')))
    mach = strip_comments(read(os.path.join(mach_dir, 'universal7580-cod3022x.c')))

    defines = {}
    for blob in (cod_h, cod_c, s28_h, s28_c):
        extract_defines(blob, defines)

    controls = {}          # name -> dict(kind, range, enum)
    enum_texts = {}        # texts variable -> [texts]

    # --- COD3022X -------------------------------------------------------
    body = array_body(cod_c, 'cod3022x_snd_controls')
    for name in CTRL_RE.findall(body):
        controls[name] = {'kind': 'scalar'}

    texts, enum_of_ctrl, enum_var_texts = parse_enums(cod_c)
    enum_texts.update(texts)

    for name, rng in parse_scalar_ranges(body, defines).items():
        if name in controls:
            controls[name]['range'] = rng
    for name, evar in enum_of_ctrl.items():
        tv = enum_var_texts.get(evar)
        if name in controls and tv:
            controls[name].update(kind='enum', enum=tv)

    for name in widget_controls(cod_c):
        controls[name] = {'kind': 'bool'}

    # --- S2801X (aux codec, ASoC name_prefix "S2801") -------------------
    m = re.search(r'\.name_prefix\s*=\s*"([^"]+)"', mach)
    prefix = (m.group(1) + ' ') if m else ''

    s28_texts, s28_enum_of_ctrl, s28_enum_var_texts = parse_enums(s28_c)
    enum_texts.update(s28_texts)

    body = array_body(s28_c, 's2801x_snd_controls')
    for name in CTRL_RE.findall(body):
        controls[prefix + name] = {'kind': 'scalar'}
    for name, rng in parse_scalar_ranges(body, defines).items():
        key = prefix + name
        if key in controls:
            controls[key]['range'] = rng
    for name, evar in s28_enum_of_ctrl.items():
        tv = s28_enum_var_texts.get(evar)
        key = prefix + name
        if key in controls and tv:
            controls[key].update(kind='enum', enum=tv)

    for name in widget_controls(s28_c, prefix):
        controls[name] = {'kind': 'bool'}

    return controls, enum_texts, kdir


def rom_controls(path):
    tree = ET.parse(path)
    used = []
    for el in tree.iter('ctl'):
        used.append((el.get('name'), el.get('value')))
    return used


def ucm_csets(paths):
    pat = re.compile(r'cset\s+"name=\'([^\']+)\'\s*(.*?)"\s*$', re.M)
    out = []
    for p in paths:
        for m in pat.finditer(read(p)):
            value = m.group(2).strip()
            if len(value) >= 2 and value[0] == "'" and value[-1] == "'":
                value = value[1:-1]
            out.append((p, m.group(1), value))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
    args = ap.parse_args()
    root = args.root

    controls, enum_texts, kdir = collect_kernel_controls(root)
    rom = os.path.join(root, 'parity/userspace/audio/rom/mixer_paths.xml')
    used = rom_controls(rom)

    print('kernel tree:      %s' % kdir)
    print('kernel controls:  %d' % len(controls))
    print('ROM <ctl> uses:   %d (%d distinct)' % (used.__len__(), len({n for n, _ in used})))

    missing = sorted({n for n, _ in used if n not in controls})
    print('\n[1] ROM control names missing from the kernel: %d' % len(missing))
    for n in missing:
        print('    MISSING: %s' % n)

    confs = sorted(glob.glob(os.path.join(root, 'parity/userspace/audio/ucm2/**/*.conf'),
                             recursive=True))
    csets = ucm_csets(confs)
    missing_ucm = sorted({n for _, n, _ in csets if n not in controls})
    print('\n[2] UCM files checked: %d, cset lines: %d' % (len(confs), len(csets)))
    print('    UCM control names missing from the kernel: %d' % len(missing_ucm))
    for n in missing_ucm:
        print('    MISSING: %s' % n)

    # value validation
    bad_values = []
    for path, name, value in csets:
        info = controls.get(name)
        if not info:
            continue
        if info['kind'] == 'bool':
            if value not in ('on', 'off', '1', '0'):
                bad_values.append((name, value, 'expected on/off'))
        elif info['kind'] == 'enum':
            texts = enum_texts.get(info['enum'], [])
            if value not in texts:
                bad_values.append((name, value, 'enum texts: %s' % ', '.join(texts)))
        elif info['kind'] == 'scalar' and 'range' in info:
            try:
                iv = int(value)
            except ValueError:
                continue      # numeric check only for numeric values
            lo, hi = info['range']
            if not lo <= iv <= hi:
                bad_values.append((name, value, 'range %d..%d' % (lo, hi)))

    print('\n[3] UCM textual/numeric values outside declared range: %d' % len(bad_values))
    for name, value, why in bad_values:
        print('    BAD: %-32s %-12s (%s)' % (name, value, why))

    # show which controls the UCM uses
    print('\nUCM control names used: %d distinct' % len({n for _, n, _ in csets}))
    for n in sorted({n for _, n, _ in csets}):
        print('    %-36s %s' % (n, controls.get(n, {}).get('kind', 'MISSING')))

    ok = not missing and not missing_ucm and not bad_values
    print('\nRESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
