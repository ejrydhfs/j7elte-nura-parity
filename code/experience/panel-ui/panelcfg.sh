#!/bin/sh
F=/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc
python3 - "$F" <<'PY'
import sys,re
p=sys.argv[1]
s=open(p).read()
for cid,th in (("2","88"),("3","44")):
    key="[Containments][%s][General]"%cid
    if key not in s:
        s=s.rstrip()+f"\n\n{key}\nthickness={th}\n"
    else:
        s=re.sub(re.escape(key)+r"\n(?:.*\n)*?(?=\n\[|\Z)", key+f"\nthickness={th}\n", s, count=1)
open(p,"w").write(s)
print("ok")
PY
grep -n "thickness" $F
sh /home/user/cleanstart15.sh >/dev/null 2>&1
