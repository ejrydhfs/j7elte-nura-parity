// Make every plasmashell window fullscreen on the single output.
function apply() {
    let ws = workspace;
    let list = ws.windowList ? ws.windowList() : (ws.windows || []);
    for (let i = 0; i < list.length; i++) {
        let w = list[i];
        let rc = w.resourceClass ? String(w.resourceClass) : "";
        if (rc.indexOf("plasmashell") !== -1 || rc.indexOf("plasma") !== -1) {
            w.fullScreen = true;
            w.noBorder = true;
        }
    }
}
apply();
