import os, sys, numpy as np, cramjam, bson
from multiprocessing import Pool
# Hawk-Eye フレーム BSON のフォルダ（引数で指定。省略時はプロジェクトの Data/HawkEye）
D=sys.argv[1] if len(sys.argv)>1 else os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','Data','HawkEye')
def load(f):
    return bson.decode(bytes(cramjam.snappy.decompress_raw(open(os.path.join(D,f),'rb').read())))
def proc(f):
    d=load(f); t=d['time']; out=[]
    for p in d['personFrameData']['people']:
        if p.get('roleId')!=1: 
            if p.get('roleId')==3: pass
            else: continue
        pos=np.frombuffer(p['positions'],np.float32)
        ids=np.frombuffer(p['jointIds'],np.uint8)
        J=np.full((29,3),np.nan,np.float32)
        J[ids]=pos.reshape(-1,3)
        keys=set(p.keys())
        out.append((t,p.get('trackId',-1),p.get('teamId',-1),p.get('roleId',-1),p.get('jerseyNumber',p.get('jersey',-1)),J))
    return t,out
if __name__=='__main__':
    fs=sorted(f for f in os.listdir(D) if f.endswith('.bson'))
    d=load(fs[len(fs)//2]); print(d['personFrameData']['people'][0].keys())
    with Pool(os.cpu_count()) as pool:
        res=pool.map(proc,fs,chunksize=500)
    T=np.array([r[0] for r in res],np.int64)
    rows=[(i,)+x[1:5] for i,r in enumerate(res) for x in r[1]]
    Js=np.stack([x[5] for r in res for x in r[1]])
    a=np.array(rows,np.int64)
    np.savez_compressed('f9_people.npz',T=T,frame=a[:,0],track=a[:,1],team=a[:,2],role=a[:,3],jersey=a[:,4],J=Js)
    print(len(T),a.shape,np.unique(a[:,2],return_counts=True),np.unique(a[:,3],return_counts=True),np.unique(a[:,4],return_counts=True))
