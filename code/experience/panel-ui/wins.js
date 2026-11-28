const ws = workspace.windowList();
print("WINS count=" + ws.length);
for (let i = 0; i < ws.length; i++) {
    const w = ws[i];
    print("WIN " + i +
          " class=" + w.resourceClass +
          " caption=" + w.caption +
          " layer=" + w.layer +
          " geo=" + w.frameGeometry.x + "," + w.frameGeometry.y + " " + w.frameGeometry.width + "x" + w.frameGeometry.height +
          " out=" + (w.output ? w.output.name : "none") +
          " min=" + w.minimized +
          " onDesk=" + w.onAllDesktops +
          " active=" + w.active +
          " pid=" + w.pid);
}
const outs = workspace.outputList ? workspace.outputList() : [];
print("OUTS count=" + outs.length);
for (let i = 0; i < outs.length; i++) print("OUT " + outs[i].name + " " + outs[i].geometry.width + "x" + outs[i].geometry.height + " enabled=" + outs[i].enabled);
