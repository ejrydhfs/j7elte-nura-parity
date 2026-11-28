p = "/home/user/cs21.sh"
s = open(p).read()
s = s.replace("sh -c 'kwin_wayland --drm", "setsid nohup sh -c 'kwin_wayland --drm")
if "setsid nohup sh -c 'plasmashell" not in s and "plasmashell > /data/clean-pshell.log" in s:
    s = s.replace("sh -c 'plasmashell > /data/clean-pshell.log",
                  "setsid nohup sh -c 'plasmashell > /data/clean-pshell.log")
open(p, "w").write(s)
print([l for l in s.splitlines() if "kwin_wayland --drm" in l or "plasmashell >" in l])
