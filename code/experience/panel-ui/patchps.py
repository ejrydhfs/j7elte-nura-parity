p = "/home/user/cs21.sh"
s = open(p).read()
old = "nohup plasmashell > /data/clean-pshell.log 2>&1 &"
new = ("sh -c 'plasmashell > /data/clean-pshell.log 2>&1; echo PLASMA_EXIT="
       + chr(36) + "? > /data/plasma.exit' >/dev/null 2>&1 </dev/null &")
if "PLASMA_EXIT" not in s:
    assert old in s, s[s.index("plasmashell")-40:s.index("plasmashell")+80]
    open(p, "w").write(s.replace(old, new, 1))
    print("wrapped plasmashell")
else:
    print("already")
