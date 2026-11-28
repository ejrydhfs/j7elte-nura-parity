import re
p="/data/build/plasma-mobile-6.8.3/containments/panel/qml/main.qml"
s=open(p).read()
old=s[s.index("    // Whether the currently showing app is in \"fullscreen\""):s.index("    property WindowPlugin.WindowMaximizedTracker")]
new='''    // Force the status bar visible: the mobile shell's fullscreen detection
    // reports the desktop window itself as fullscreen on this setup, which
    // hid the panel on every startup.
    readonly property bool fullscreen: false
    onFullscreenChanged: {
        MobileShellState.ShellDBusClient.panelState = "default";
    }

'''
s=s.replace(old,new,1)
s=s.replace('readonly property bool showingStartupFeedback: MobileShellState.ShellDBusObject.startupFeedbackModel.activeWindowIsStartupFeedback',
            'readonly property bool showingStartupFeedback: MobileShellState.ShellDBusObject ? MobileShellState.ShellDBusObject.startupFeedbackModel.activeWindowIsStartupFeedback : false')
open(p,"w").write(s)
print("patched")
