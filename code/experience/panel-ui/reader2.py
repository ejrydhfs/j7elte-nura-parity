import socket, time, threading
def run(i, out):
    s=socket.create_connection(('192.168.0.8',47777), timeout=5)
    s.sendall(b'GET /v1/stream?camera=back&width=1280&height=720&fps=30 HTTP/1.1\r\nHost: x\r\n\r\n')
    buf=b''; n=0; end=time.time()+8
    while time.time()<end:
        b=s.recv(65536)
        if not b: break
        buf+=b; n+=len(b)
    out[i]=n
out={}
ts=[threading.Thread(target=run,args=(i,out)) for i in range(2)]
[t.start() for t in ts]; [t.join() for t in ts]
print('two clients bytes', out)
