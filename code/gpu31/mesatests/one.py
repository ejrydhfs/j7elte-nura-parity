import json, bz2
d = json.load(bz2.open("/data/mesatests/piglit-results/results.json.bz2"))
ts = d["tests"]
k = "shaders@glsl-vs-arrays"
v = ts[k]
for f in ("result", "returncode", "command", "out", "err", "dmesg", "time"):
    print(f.upper(), ":", repr(v.get(f))[:1500])
