// Enumerate KWin's scene: captions, geometry, layer, type.
const ws = workspace;
const wins = ws.windowList();
print("PROBE windows=" + wins.length);
for (let i = 0; i < wins.length; i++) {
    const w = wins[i];
    let g = "";
    try { g = w.frameGeometry.x + "," + w.frameGeometry.y + " " + w.frameGeometry.width + "x" + w.frameGeometry.height; } catch (e) { g = "?"; }
    let layer = -1, type = -1, vis = false, cap = "";
    try { layer = w.layer; } catch (e) {}
    try { type = w.type; } catch (e) {}
    try { vis = w.visible; } catch (e) {}
    try { cap = w.caption; } catch (e) {}
    print("WIN[" + i + "] cap='" + cap + "' geom=" + g + " layer=" + layer + " type=" + type + " visible=" + vis);
}
