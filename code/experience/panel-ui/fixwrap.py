p = "/home/user/cs21.sh"
lines = open(p).read().splitlines()
good = ("sh -c 'kwin_wayland --drm --no-lockscreen --socket wayland-0 "
        "> /data/clean-kwin.log 2>&1; echo KWIN_EXIT=" + chr(36) + "? "
        "> /data/kwin.exit' >/dev/null 2>&1 </dev/null &")
out = [good if "KWIN_EXIT" in l else l for l in lines]
open(p, "w").write("\n".join(out) + "\n")
print([l for l in open(p).read().splitlines() if "KWIN_EXIT" in l])
