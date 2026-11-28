import re
p = "/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc"
s = open(p).read()
s = re.sub(r'(?m)^wallpaperplugin=.*$', 'wallpaperplugin=org.kde.image', s)
img = "file:///usr/share/wallpapers/postmarketos.jpg"
for n in (1, 2, 3):
    hdr = f"[Containments][{n}][Wallpaper][org.kde.image][General]"
    if hdr not in s:
        s = s.rstrip() + f"\n\n{hdr}\nImage={img}\n"
    else:
        s = re.sub(re.escape(hdr) + r'\n(?!Image=)', hdr + f"\nImage={img}\n", s)
open(p, "w").write(s)
print("wallpaperplugin lines:", [l for l in s.splitlines() if l.startswith("wallpaperplugin=")])
print("image lines:", [l for l in s.splitlines() if l.startswith("Image=")])
