import json, bz2
d = json.load(bz2.open("/data/mesatests/piglit-results/results.json.bz2"))
ts = d["tests"]
for k in list(ts)[:4]:
    v = ts[k]
    print("=" * 70)
    print("TEST:", k)
    for f in ("result", "returncode", "command", "out", "err", "environment"):
        if f in v:
            s = str(v[f])
            print(f.upper(), ":", s[:1000])
