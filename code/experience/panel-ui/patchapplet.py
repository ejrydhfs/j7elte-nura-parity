p = "/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s = open(p).read()
needle = 'console.log("PANELGEOM id="'
i = s.index(needle); j = s.index('\n', i)
new = ('console.log("PANELGEOM id=" + (Plasmoid.containment ? Plasmoid.containment.id : -1)'
       ' + " CPANEL=" + (Plasmoid.containment && Plasmoid.containment.panel ? "set" : "null")'
       ' + " RPANEL=" + (root.panel ? "set" : "null")'
       ' + " h=" + root.panel.height + " geo=" + root.panel.x + "," + root.panel.y'
       ' + " " + root.panel.width + "x" + root.panel.height);')
open(p, "w").write(s[:i] + new + s[j:])
print("applet log updated")
