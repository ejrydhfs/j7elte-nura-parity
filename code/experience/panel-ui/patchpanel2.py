p = "/usr/share/plasma/shells/org.kde.plasma.mobileshell/contents/views/Panel.qml"
s = open(p).read()
if "PANELINJECT" not in s:
    anchor = '        console.log("SHELLPANEL completed'
    # insert a deterministic injector right before the completion handler's log
    add = '''    // PANELINJECT: containment can arrive after Component.onCompleted, so keep
    // trying until the PanelView has been handed to it.  Without this the
    // assignment was timing-dependent (containment=null at completion).
    property bool panelInjected: (containment && containment.panel) ? true : false
    Timer {
        interval: 100
        repeat: true
        running: !root.panelInjected
        onTriggered: {
            if (!root.containment)
                return;
            root.containment.panel = panel;
            root.panelInjected = true;
            console.log("PANELINJECT panel=" + panel + " containment=" + root.containment);
        }
    }

'''
    assert anchor in s
    s = s.replace(anchor, add + anchor, 1)
    open(p, "w").write(s)
    print("added injector")
else:
    print("already")
