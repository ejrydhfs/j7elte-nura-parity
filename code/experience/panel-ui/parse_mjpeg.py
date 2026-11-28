import sys
def frames(path):
    d=open(path,'rb').read(); out=[]; i=0
    pat=b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: '
    while True:
        j=d.find(pat,i)
        if j<0: break
        k=j+len(pat); e=d.find(b'\r\n',k)
        if e<0: break
        n=int(d[k:e]); start=e+2
        if d[start:start+2]==b'\r\n': start+=2
        body=d[start:start+n]
        if len(body)==n and body[:2]==b'\xff\xd8' and body[-2:]==b'\xff\xd9': out.append(body)
        i=start+n
    return out
if __name__=='__main__':
    fs=frames(sys.argv[1])
    if fs: open(sys.argv[2],'wb').write(fs[-1])
    print(f'frames={len(fs)} last={len(fs[-1]) if fs else 0}')
