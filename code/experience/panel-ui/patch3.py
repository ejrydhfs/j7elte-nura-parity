p = "/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s = open(p).read()
# 1) window-sized fill instead of parent-anchored
old_fill = 'Rectangle { anchors.fill: parent; color: "#0000ff"; z: -100 }'
new_fill = ('Rectangle { x: 0; y: 0; '
            'width: Window.window ? Window.window.width : 0; '
            'height: Window.window ? Window.window.height : 0; '
            'color: "#0000ff"; z: -100 }')
if old_fill in s:
    s = s.replace(old_fill, new_fill, 1)
# 2) log containment item size
i = s.index('console.log("PANELGEOM')
j = s.index('\n', i)
line = s[i:j]
if ' root=' not in line:
    line = line[:-2] + ' + " root=" + root.width + "x" + root.height);'
    s = s[:i] + line + s[j:]
open(p, "w").write(s)
print("fill:", new_fill[:60], "...")
print("log:", line[:120], "...")
