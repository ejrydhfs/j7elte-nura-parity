var l = (workspace.windowList ? workspace.windowList() : (workspace.windows || []));
for (var i = 0; i < l.length; i++) {
    var w = l[i];
    if (("" + w.resourceClass).indexOf("qml6") >= 0) {
        (function(win){
            var n = 0;
            var t = setInterval(function(){
                var g = win.frameGeometry;
                win.frameGeometry = Qt.rect(g.x + ((n++ % 2) ? 40 : -40), g.y, g.width, g.height);
                if (n > 30) clearInterval(t);
            }, 300);
        })(w);
        console.log("MOVE started");
    }
}
