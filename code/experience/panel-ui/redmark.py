for p in ("/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml",
          "/data/build/plasma-mobile-6.8.3/containments/taskpanel/qml/main.qml"):
    s = open(p).read()
    if "color: \"#ff0000\"" in s:
        continue
    marker = '''
    // visual marker for bring-up debugging
    Rectangle {
        anchors.fill: parent
        color: "#ff0000"
        z: -1
    }
'''
    i = s.index("ContainmentItem {")
    j = s.index("{", i) + 1
    s = s[:j] + marker + s[j:]
    open(p, "w").write(s)
print("ok")
