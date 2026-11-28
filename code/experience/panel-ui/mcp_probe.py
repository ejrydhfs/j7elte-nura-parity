import json, subprocess, sys, time, os
env=dict(os.environ, OPENCAMO_URL="http://192.168.0.8:47777")
p=subprocess.Popen([sys.executable,"mcp/opencamo_mcp.py"],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env,bufsize=1)
def send(obj):
    p.stdin.write(json.dumps(obj)+"\n"); p.stdin.flush()
def read(timeout=20):
    import select
    r,_,_=select.select([p.stdout],[],[],timeout)
    if not r: return None
    return json.loads(p.stdout.readline())
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}})
print("init", read()["result"]["serverInfo"]["name"])
send({"jsonrpc":"2.0","method":"notifications/initialized"})
send({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"status","arguments":{}}})
r=read(); print("status:", json.dumps(r.get("result",r))[:300])
send({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"snapshot","arguments":{"camera":"back","grid":True,"max_width":512}}})
r=read()
content=r.get("result",{}).get("content",[])
print("snapshot content types:", [c.get("type") for c in content])
for c in content:
    if c.get("type")=="image": print("image bytes(b64 chars):", len(c.get("data","")))
    if c.get("type")=="text": print("text:", c.get("text")[:100])
send({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"focus_distance","arguments":{"diopters":1.5}}})
r=read(); print("focus_distance:", json.dumps(r.get("result",r))[:200])
send({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"focus","arguments":{"x":0.25,"y":0.75}}})
r=read(); print("focus:", json.dumps(r.get("result",r))[:200])
p.stdin.close(); p.wait(timeout=10)
print("stderr:", p.stderr.read()[:300])
