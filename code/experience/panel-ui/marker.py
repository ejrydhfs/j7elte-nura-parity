for p in ("/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml",
          "/data/build/plasma-mobile-6.8.3/containments/taskpanel/qml/main.qml"):
    s = open(p).read()
    s = s.replace("Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground",
                  "Plasmoid.backgroundHints: PlasmaCore.Types.DefaultBackground")
    s = s.replace("readonly property real panelHeight: MobileShell.Constants.topPanelHeight",
                  "readonly property real panelHeight: 200")
    open(p, "w").write(s)
print("patched")
