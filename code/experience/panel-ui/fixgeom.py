p = "/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s = open(p).read()
old_start = 'console.log("PANELGEOM'
i = s.index(old_start)
j = s.index('\n', i)
new = ('console.log("PANELGEOM id=" + (Plasmoid.containment ? Plasmoid.containment.id : -1)'
       ' + " h=" + root.panel.height + " geo=" + root.panel.x + "," + root.panel.y'
       ' + " " + root.panel.width + "x" + root.panel.height);')
s = s[:i] + new + s[j:]
open(p, "w").write(s)
print("fixed:", new)
