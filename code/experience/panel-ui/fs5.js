var ws = workspace;
var list = (ws.windowList ? ws.windowList() : (ws.windows || []));
for (var i = 0; i < list.length; i++) {
    var w = list[i];
    var rc = w.resourceClass ? String(w.resourceClass) : "";
    var g = w.frameGeometry;
    console.log("WIN cls=" + rc + " cap=" + w.caption + " geom=" + g.x + "," + g.y + " " + g.width + "x" + g.height +
                " min=" + w.minimized + " skip=" + w.skipTaskbar + " above=" + w.keepAbove);
}
