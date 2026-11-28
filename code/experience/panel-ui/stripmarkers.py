import re
base = "/data/build/plasma-mobile-6.8.3/containments/"
for f in ("panel/qml/main.qml", "taskpanel/qml/main.qml"):
    p = base + f
    s = open(p).read()
    # drop the bring-up marker rectangle (anchors.fill + colour + z: 5)
    s = re.sub(r'\n\s*// visual marker for bring-up debugging\n', '\n', s)
    s = re.sub(r'\n\s*Rectangle \{\n(?:[^\n]*\n){0,4}?\s*z: 5\n\s*\}\n', '\n', s)
    # drop the forced fill line
    s = re.sub(r'\n[^\n]*color: "#0000ff"; z: -100 \}[^\n]*', '\n', s)
    open(p, "w").write(s)
    print(f, "ff0000" in s, "00ff00" in s, "0000ff" in s)
