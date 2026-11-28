p="/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc"
s=open(p).read()
import re
# ensure containment 1 uses the colour wallpaper
s=re.sub(r'(plugin=org\.kde\.plasma\.mobile\.homescreen\.folio\n)wallpaperplugin=[^\n]*', r'\1wallpaperplugin=org.kde.color', s, count=1)
if "[Containments][1][Wallpaper][org.kde.color][General]" in s:
    s=re.sub(r'\[Containments\]\[1\]\[Wallpaper\]\[org\.kde\.color\]\[General\]\nColor=[^\n]*',
             '[Containments][1][Wallpaper][org.kde.color][General]\nColor=0,255,0', s)
else:
    s=s.rstrip()+"\n\n[Containments][1][Wallpaper][org.kde.color][General]\nColor=0,255,0\n"
open(p,"w").write(s)
print("ok")
