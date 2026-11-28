// Raise the 720x56 panel window and report its layer before/after.
const wins = workspace.windowList();
for (let i = 0; i < wins.length; i++) {
    const w = wins[i];
    let g = "";
    try { g = w.frameGeometry.width + "x" + w.frameGeometry.height; } catch (e) { g = "?"; }
    if (g === "720x56") {
        print("RAISE before layer=" + w.layer + " keepAbove=" + w.keepAbove);
        w.keepAbove = true;
        print("RAISE after keepAbove set");
    }
}
