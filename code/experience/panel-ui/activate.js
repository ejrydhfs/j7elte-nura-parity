const ws = workspace;
const list = (ws.windowList ? ws.windowList() : ws.clientList());
for (let i = 0; i < list.length; i++) {
    const c = list[i];
    if ((c.caption || "").indexOf("opencamo") >= 0) {
        c.activate ? c.activate(true, true) : (ws.activeWindow = c);
    }
}
