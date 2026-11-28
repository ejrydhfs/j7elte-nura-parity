p="/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s=open(p).read()
if "enforceTimer" not in s:
    marker='''    // diagnostic: re-apply the panel window properties periodically; the shell
    // hides the panel again shortly after the initial setWindowProperties().
    Timer {
        id: enforceTimer
        interval: 1500
        running: true
        repeat: true
        onTriggered: root.setWindowProperties()
    }
'''
    i=s.index("    property var panel: null")
    j=s.index("\n", i)+1
    s=s[:j]+marker+s[j:]
open(p,"w").write(s)
print("ok")
