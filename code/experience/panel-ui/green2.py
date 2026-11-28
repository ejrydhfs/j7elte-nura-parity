p="/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc"
s=open(p).read()
s=s.replace("Color=0,255,0","Color=#00ff00")
open(p,"w").write(s)
print("ok")
