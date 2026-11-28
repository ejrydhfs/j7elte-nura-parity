const ws = workspace.windowList();
print("WINS2 count=" + ws.length);
for (let i = 0; i < ws.length; i++) {
    const w = ws[i];
    print("WIN2 " + i + " class=" + w.resourceClass +
          " layer=" + w.layer +
          " geo=" + w.frameGeometry.x + "," + w.frameGeometry.y + " " + w.frameGeometry.width + "x" + w.frameGeometry.height +
          " opacity=" + w.opacity +
          " visible=" + w.visible +
          " minimized=" + w.minimized +
          " hidden=" + w.hidden +
          " onAllDesktops=" + w.onAllDesktops +
          " active=" + w.active);
}
