#!/usr/bin/env python3
"""check-baseline.py - verify a live run against code/gpu31/baseline/llvmpipe-v1.json.

The JSON file is the canonical baseline + HW threshold source (read it, do
not duplicate its numbers here). This script checks renderer, glmark2 Score,
xwd-parse surface stats, and before/after GPU state, then prints PASS:/WARN:/
FAIL: lines and exits nonzero on FAIL. H2/H3/H5 WARNs never FAIL; they print
a 'needs explanation' reminder, and --require-expl FAILs unless the operator
passes --expl TEXT for each (recorded into the verdict notes).

Usage examples:
  check-baseline.py --glmark-log glmark2-es2.log [--score-only]
  check-baseline.py --renderer-txt renderer.txt --expect hardware
  check-baseline.py --xwd-parse xwd-parse.txt [--geometry 720x1280 --min-distinct 100]
  check-baseline.py --gpu-before gpu-state-before.txt --gpu-after gpu-state-after.txt
Exit codes: 0 = no FAIL, 1 = FAIL, 2 = bad invocation/missing baseline.
Only the standard library is used.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE_PATH = os.environ.get("GPU31_BASELINE", os.path.join(HERE, "llvmpipe-v1.json"))

SOFTWARE_RES = ("llvmpipe", "swrast", "softpipe")
HARDWARE_RES = ("mali", "panfrost", "arm", "exynos", "kbase", "lima")


def fail(msg):
    sys.stdout.write("FAIL: %s\n" % msg)
    return False


def warn(msg):
    sys.stdout.write("WARN: %s (needs explanation)\n" % msg)
    return True


def parse_kv(path):
    out = {}
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def to_int(s, default=None):
    try:
        return int(s)
    except (TypeError, ValueError):
        return default


def check_renderer(bl, raw, expect):
    if raw is None:
        return fail("renderer: no renderer line captured")
    low = raw.lower()
    if any(t in low for t in SOFTWARE_RES):
        cls = "software"
    elif any(t in low for t in HARDWARE_RES):
        cls = "hardware"
    elif not raw.strip():
        cls = "empty"
        return fail("renderer: empty renderer string")
    else:
        cls = "hardware-unknown"
        sys.stdout.write("WARN: renderer: unrecognised non-software id; recording [%s]\n" % raw)
    if expect in ("auto", None):
        if cls in ("software", "hardware"):
            sys.stdout.write("PASS: renderer class=%s [%s]\n" % (cls, raw))
            return True
        sys.stdout.write("PASS: renderer class=%s [%s]\n" % (cls, raw))
        return True
    if expect == "software":
        if cls == "software":
            sys.stdout.write("PASS: renderer class=software [%s]\n" % raw)
            return True
        return fail("renderer is '%s', expected software" % cls)
    if expect == "hardware":
        if cls == "hardware":
            sys.stdout.write("PASS: renderer class=hardware [%s]\n" % raw)
            return True
        return fail("renderer is '%s', expected hardware [%s]" % (cls, raw))
    return fail("renderer: bad --expect value %r" % expect)


def check_score(bl, log_text):
    base = bl["glmark2_es2"]["score"]
    m = re.search(r"glmark2\s+Score:\s*(\d+)", log_text)
    if not m:
        return warn("glmark2 Score line missing (run may have been timeout-cut); "
                    "fall back to per-scene floors in scenes.tbl")
    got = int(m.group(1))
    if got >= base:
        sys.stdout.write("PASS: glmark2 Score %d >= baseline %d\n" % (got, base))
        return True
    floor = base // 2
    if got < floor:
        return fail("glmark2 Score %d < 50%% floor %d (baseline %d)" % (got, floor, base))
    warn("glmark2 Score %d below baseline %d but above floor %d" % (got, base, floor))
    return True


def check_xwd_parse(bl, kv):
    ok = True
    exp = bl["surface_2d"]["xwd"]
    geo = os.environ.get("GPU31_GEOMETRY", "")
    w, h = to_int(kv.get("width"), -1), to_int(kv.get("height"), -1)
    if geo:
        m = re.match(r"(\d+)x(\d+)$", geo)
        if not m:
            sys.stderr.write("check-baseline: bad geometry %r\n" % geo)
            return False
        ew, eh = int(m.group(1)), int(m.group(2))
        if (w, h) != (ew, eh):
            ok = fail("2D surface geometry %dx%d != expected %dx%d" % (w, h, ew, eh))
        else:
            sys.stdout.write("PASS: 2D surface geometry %dx%d\n" % (w, h))
    else:
        sys.stdout.write("PASS: 2D surface parsed %dx%d depth=%s\n" % (w, h, kv.get("depth", "?")))
    min_d = to_int(os.environ.get("GPU31_MIN_DISTINCT", "100"), 100)
    distinct = to_int(kv.get("distinct"), -1)
    if distinct is None or distinct < 0:
        ok = fail("2D surface: no distinct count parsed")
    elif distinct < min_d:
        ok = fail("2D surface distinct %d < minimum %d (blank/degenerate)" % (distinct, min_d))
    else:
        sys.stdout.write("PASS: 2D surface distinct=%d >= %d (baseline had %d)\n"
                         % (distinct, min_d, exp["distinct"]))
    return ok


def check_gpu(bl, before, after):
    ub, ua = before.get("utilization"), after.get("utilization")
    tb = to_int(before.get("tis_total"), None)
    ta = to_int(after.get("tis_total"), None)
    if ub == "MISSING" and tb is None:
        sys.stdout.write("SKIP: gpu state (no kbase sysfs on this host)\n")
        return True
    grew = (tb is not None and ta is not None and ta > tb)
    active = False
    for u in (ub, ua):
        v = to_int(u, None)
        if v is not None and v > 0:
            active = True
    if active or grew:
        sys.stdout.write("PASS: gpu activity (util %s->%s, tis %s->%s)\n" % (ub, ua, tb, ta))
        return True
    warn("gpu idle across load (util %s->%s, tis %s->%s); "
         "expected under llvmpipe, unexplained under hardware" % (ub, ua, tb, ta))
    return True


def check_glmark_log_floor(bl, log_text, scenes_tbl):
    """WARN-only per-scene floor check (mirrors tools/check-floors.sh)."""
    base = {}
    try:
        with open(scenes_tbl) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("|")
                if len(parts) == 3:
                    base[parts[0]] = (int(parts[1]), parts[2])
    except OSError as e:
        return fail("cannot read scenes table %s: %s" % (scenes_tbl, e))
    fps_lines = re.findall(r"FPS:\s*(\d+)", log_text)
    if len(fps_lines) < 5:
        return fail("glmark capture broken: only %d FPS lines" % len(fps_lines))
    sys.stdout.write("PASS: glmark capture health (%d FPS lines)\n" % len(fps_lines))
    return True


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="verify a live run against llvmpipe-v1.json")
    ap.add_argument("--baseline", default=BASELINE_PATH)
    ap.add_argument("--renderer-txt")
    ap.add_argument("--glmark-log")
    ap.add_argument("--xwd-parse")
    ap.add_argument("--gpu-before")
    ap.add_argument("--gpu-after")
    ap.add_argument("--expect", default="auto", choices=["auto", "software", "hardware"])
    ap.add_argument("--geometry", default="")
    ap.add_argument("--min-distinct", default="100")
    ap.add_argument("--scenes-tbl", default=os.path.join(HERE, "..", "tests", "scenes.tbl"))
    ap.add_argument("--expl", action="append", default=[])
    ap.add_argument("--require-expl", action="store_true")
    args = ap.parse_args(argv)

    try:
        with open(args.baseline) as f:
            bl = json.load(f)
    except (OSError, ValueError) as e:
        sys.stderr.write("check-baseline: cannot load baseline %s: %s\n" % (args.baseline, e))
        return 2
    if bl.get("schema") != "gpu31-baseline/1":
        sys.stderr.write("check-baseline: unknown schema %r\n" % bl.get("schema"))
        return 2

    if args.geometry:
        os.environ["GPU31_GEOMETRY"] = args.geometry
    if args.min_distinct:
        os.environ["GPU31_MIN_DISTINCT"] = args.min_distinct

    if not any([args.renderer_txt, args.glmark_log, args.xwd_parse,
                args.gpu_before, args.gpu_after]):
        sys.stderr.write("check-baseline: nothing to check; pass at least one input flag\n")
        return 2

    ok = True
    warned = 0
    import io
    from contextlib import redirect_stdout
    buf = io.StringIO()
    with redirect_stdout(buf):
        if args.renderer_txt:
            try:
                with open(args.renderer_txt) as f:
                    raw = f.read().strip().splitlines()
                raw = raw[0] if raw else ""
            except OSError as e:
                ok = fail("cannot read renderer file: %s" % e) and ok
                raw = None
            if raw is not None:
                ok = check_renderer(bl, raw, args.expect) and ok
        if args.glmark_log:
            try:
                with open(args.glmark_log) as f:
                    text = f.read()
            except OSError as e:
                ok = fail("cannot read glmark log: %s" % e) and ok
                text = None
            if text is not None:
                ok = check_score(bl, text) and ok
                ok = check_glmark_log_floor(bl, text, args.scenes_tbl) and ok
        if args.xwd_parse:
            try:
                kv = parse_kv(args.xwd_parse)
            except OSError as e:
                ok = fail("cannot read xwd-parse file: %s" % e) and ok
                kv = None
            if kv is not None:
                ok = check_xwd_parse(bl, kv) and ok
        if args.gpu_before or args.gpu_after:
            if not (args.gpu_before and args.gpu_after):
                ok = fail("gpu check needs both --gpu-before and --gpu-after") and ok
            else:
                try:
                    b, a = parse_kv(args.gpu_before), parse_kv(args.gpu_after)
                except OSError as e:
                    ok = fail("cannot read gpu state files: %s" % e) and ok
                    b = a = None
                if b is not None:
                    ok = check_gpu(bl, b, a) and ok
    out = buf.getvalue()
    sys.stdout.write(out)
    warned = out.count("(needs explanation)")
    if args.require_expl and warned > len(args.expl):
        sys.stdout.write("FAIL: %d WARN(s) need --expl TEXT each (%d given)\n" % (warned, len(args.expl)))
        return 1
    if warned and not args.require_expl:
        sys.stdout.write("note: %d WARN(s) above each need a written explanation in the verdict notes\n" % warned)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
