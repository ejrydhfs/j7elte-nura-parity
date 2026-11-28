import re
p="/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc"
s=open(p).read()
s=re.sub(r'(plugin=org\.kde\.plasma\.mobile\.homescreen\.folio\n)wallpaperplugin=[^\n]*',
         r'\1wallpaperplugin=org.kde.image', s, count=1)
if "[Containments][1][Wallpaper][org.kde.image][General]" not in s:
    s=s.rstrip()+"\n\n[Containments][1][Wallpaper][org.kde.image][General]\nImage=file:///usr/share/wallpapers/postmarketos.jpg\nFillMode=2\n"
open(p,"w").write(s)
print("ok")
