var ws = workspace;
var list = (ws.windowList ? ws.windowList() : (ws.windows || []));
var n = 0;
for (var i = 0; i < list.length; i++) {
    var w = list[i];
    var rc = w.resourceClass ? String(w.resourceClass) : "";
    if (rc.indexOf("qml6") !== -1) { w.keepAbove = true; n++; }
}
console.log("ABOVE", n);
