p="/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s=open(p).read()
old='            root.panel.visibilityMode = ShellSettings.Settings.autoHidePanelsEnabled ? 3 : 0'
new=old+'\n            // The shell may have started with the panel state "hidden"; make\n            // the status bar visible explicitly (fullscreen is forced false above).\n            MobileShellState.ShellDBusClient.panelState = "default";'
assert old in s and new not in s
open(p,"w").write(s.replace(old,new,1))
print("ok")
