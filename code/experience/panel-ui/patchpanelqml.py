p = "/usr/share/plasma/shells/org.kde.plasma.mobileshell/contents/views/Panel.qml"
s = open(p).read()
if "SHELLPANEL" in s:
    print("already patched")
else:
    anchor = "    property Item containment"
    add = anchor + """

    // diagnostic: assign the PanelView to the containment at completion too;
    // onContainmentChanged may fire before this component's Connections exist.
    Component.onCompleted: {
        console.log("SHELLPANEL completed panel=" + panel + " containment=" + containment +
                    " hasProp=" + (containment ? (containment.panel !== undefined) : "n/a"));
        if (containment && containment.panel !== undefined)
            containment.panel = panel;
    }"""
    assert anchor in s
    open(p, "w").write(s.replace(anchor, add, 1))
    print("patched Panel.qml")
