var ws = workspace;
var list = (ws.windowList ? ws.windowList() : (ws.windows || []));
for (var i = 0; i < list.length; i++) {
    var w = list[i];
    var rc = w.resourceClass ? String(w.resourceClass) : "";
    if (rc.indexOf("plasmashell") !== -1) {
        w.minimized = true;
    } else {
        w.keepAbove = true;
    }
}
console.log("MINIMIZED", list.length);
