p="/home/user/cs21.sh"
s=open(p).read()
old='kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/clean-kwin.log 2>&1 &'
new='''sh -c 'kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/clean-kwin.log 2>&1; echo KWIN_EXIT=$? > /data/kwin.exit' &'''
if old in s and "KWIN_EXIT" not in s:
    s=s.replace(old,new,1)
    print("wrapped kwin")
open(p,"w").write(s)
# disable heavy effects
k="/data/plasma-run/config/kwinrc"
try:
    c=open(k).read()
except FileNotFoundError:
    c=""
if "[Plugins]" not in c:
    c += "\n[Plugins]\n"
for e in ["blur","contrast","backgroundcontrast","screenshot","overview","fadedesktop","slide","scale"]:
    key=e+"Enabled=false"
    if key not in c:
        c = c.replace("[Plugins]\n", "[Plugins]\n"+key+"\n", 1)
open(k,"w").write(c)
print("effects disabled")
