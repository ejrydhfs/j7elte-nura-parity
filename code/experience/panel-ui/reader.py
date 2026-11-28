import socket, time, sys
s=socket.create_connection(('192.168.0.8',47777), timeout=5)
s.sendall(b'GET /v1/stream?camera=back&width=1280&height=720&fps=30 HTTP/1.1\r\nHost: x\r\n\r\n')
n=0; end=time.time()+20
buf=b''
while time.time()<end:
    b=s.recv(65536)
    if not b: break
    buf+=b; n+=len(b)
print('reader bytes', n)
