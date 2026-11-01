import struct,sys,bisect
def exports(path):
    d=open(path,'rb').read()
    pe=struct.unpack_from('<I',d,0x3c)[0]
    nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=pe+24; osz=struct.unpack_from('<H',d,pe+20)[0]
    erva=struct.unpack_from('<I',d,opt+112)[0]
    secs=[struct.unpack_from('<IIII',d,opt+osz+i*40+8) for i in range(nsec)]
    def off(r):
        for vs,va,rs,ro in secs:
            if va<=r<va+max(vs,rs): return r-va+ro
    e=off(erva); n=struct.unpack_from('<I',d,e+24)[0]
    fa,na,oa=[off(x) for x in struct.unpack_from('<III',d,e+28)]
    out=[]
    for i in range(n):
        nm=d[off(struct.unpack_from('<I',d,na+4*i)[0]):].split(b'\0')[0].decode()
        o=struct.unpack_from('<H',d,oa+2*i)[0]; out.append((struct.unpack_from('<I',d,fa+4*o)[0],nm))
    return sorted(out)
for mod in ['ntdll','ucrtbase','win32u']:
    ex=exports(f'C:/Windows/System32/{mod}.dll'); ks=[a for a,_ in ex]
    for line in open(sys.argv[1]):
        p=line.split()
        if p[0].startswith(mod+'.dll+') and int(p[1])>=25:
            r=int(p[0].split('+')[1],16); i=bisect.bisect_right(ks,r)-1
            print(p[1],mod,ex[i][1],hex(r-ex[i][0]))
