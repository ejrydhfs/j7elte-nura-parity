var ps = panels();
print("PANELS=" + ps.length);
for (var i = 0; i < ps.length; i++) {
    var p = ps[i];
    p.hiding = "none";
    print("PANEL id=" + p.id + " hiding=" + p.hiding + " visible=" + p.visible);
}
