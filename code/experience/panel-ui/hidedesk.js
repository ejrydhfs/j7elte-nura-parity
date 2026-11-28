const wins = workspace.windowList();
for (let i = 0; i < wins.length; i++) {
    const w = wins[i];
    let h = 0;
    try { h = w.frameGeometry.height; } catch (e) {}
    if (h === 1280) {
        print("HIDE desktop 720x1280 minimized before=" + w.minimized);
        w.minimized = true;
        print("HIDE done minimized=" + w.minimized);
    }
}
