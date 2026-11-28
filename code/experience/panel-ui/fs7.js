var l = (workspace.windowList ? workspace.windowList() : (workspace.windows || []));
for (var i = 0; i < l.length; i++) {
    var w = l[i];
    console.log("OUT " + w.caption + " -> " + (w.output ? w.output.name : "null") +
                " geom=" + w.frameGeometry.x + "," + w.frameGeometry.y + " " + w.frameGeometry.width + "x" + w.frameGeometry.height);
}
