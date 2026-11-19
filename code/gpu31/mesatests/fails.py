import json, bz2
d = json.load(bz2.open("/data/mesatests/piglit-results/results.json.bz2"))
ts = d["tests"]
from collections import Counter
print(Counter(v.get("result") for v in ts.values()))
print("total:", len(ts))
for k, v in ts.items():
    if v.get("result") != "pass":
        txt = str(v.get("out", v.get("err", "")))[:220].replace("\n", " | ")
        print(v.get("result"), "|", k, "|", txt)
