p="/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc"
s=open(p).read()
s=s.replace("plugin=org.kde.plasma.mobile.homescreen.folio\nwallpaperplugin=org.kde.image",
            "plugin=org.kde.plasma.mobile.homescreen.folio\nwallpaperplugin=org.kde.color",1)
group="\n[Containments][1][Wallpaper][org.kde.color][General]\nColor=0,0,0\n"
if "[Containments][1][Wallpaper][org.kde.color]" not in s:
    s=s.rstrip()+group
open(p,"w").write(s)
print("ok")
