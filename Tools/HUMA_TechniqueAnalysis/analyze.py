import numpy as np, json
z=np.load('f9_players.npz'); t=z['t']; N=len(t)
J={1:z['J1'].astype(np.float64),2:z['J2'].astype(np.float64)}
STARTS=[212.07,452.38,692.23,933.09,1173.18,1413.53,1653.92,1893.27,2133.50,2382.66]
ROUND_LEN=181.8
def round_of(x):
    for i,s in enumerate(STARTS):
        if s-0.5<=x<=s+ROUND_LEN: return i+1
    return 0
inround=np.array([round_of(x)>0 for x in t])
FPS=50.0
def sm(x,k=5):
    # moving average along time, nan-aware
    w=np.ones(k)
    v=np.nan_to_num(x); m=np.isfinite(x).astype(float)
    num=np.apply_along_axis(lambda a: np.convolve(a,w,'same'),0,v)
    den=np.apply_along_axis(lambda a: np.convolve(a,w,'same'),0,m)
    with np.errstate(invalid='ignore',divide='ignore'): r=num/den
    r[den<k*0.6]=np.nan
    return r
def head(A): return np.nanmean(A[:,[0,15,16,17,18]],axis=1)
def fist(A,s): return np.nanmean(A[:,[7,25,26]] if s=='L' else A[:,[4,27,28]],axis=1)
P={}
for jn in (1,2):
    A=J[jn]
    P[jn]=dict(head=sm(head(A),3),hip=sm(A[:,8],5),neck=sm(A[:,1],3),
      LF=sm(fist(A,'L'),3),RF=sm(fist(A,'R'),3),LS=sm(A[:,5],3),RS=sm(A[:,2],3),
      LE=sm(A[:,6],3),RE=sm(A[:,3],3),LA=sm(A[:,14],5),RA=sm(A[:,11],5),
      LW=sm(A[:,7],3),RW=sm(A[:,4],3))
Z=np.array([0,0,1.0])
def unit(v):
    n=np.linalg.norm(v,axis=-1,keepdims=True); return v/np.where(n>1e-6,n,np.nan)
frame={}
for a,b in ((1,2),(2,1)):
    d=P[b]['hip']-P[a]['hip']; d[:,2]=0; d=unit(sm(d,15))
    l=np.cross(Z,d)
    lead=np.einsum('ij,ij->i',P[a]['LA']-P[a]['RA'],d)
    ls_=sm(lead[:,None],51)[:,0]; stance=np.where(ls_>0,'O','S')  # O: orthodox (left foot lead)
    frame[a]=dict(d=d,l=l,stance=stance,lead=lead)
def ang(a,b,c):
    u=unit(a-b); v=unit(c-b); return np.degrees(np.arccos(np.clip(np.einsum('ij,ij->i',u,v),-1,1)))
events=[]
def add(jn,ts,te,kind,name,en,detail,**kw):
    r=round_of(ts)
    if r==0: return
    e=dict(t0=round(float(ts),3),t1=round(float(te),3),jersey=jn,corner='RED' if jn==1 else 'BLUE',kind=kind,name=name,en=en,detail=detail,round=r,rt=round(float(ts-STARTS[r-1]),2))
    e.update(kw); events.append(e); return e
punches={1:[],2:[]}
def segd(p,a0,b0):
    ab=b0-a0; tt=np.clip(np.einsum('ij,ij->i',p-a0,ab)/np.einsum('ij,ij->i',ab,ab),0,1)
    return np.linalg.norm(p-(a0+ab*tt[:,None]),axis=1)
VMIN=3.8
for a,b in ((1,2),(2,1)):
    pa,pb,fa=P[a],P[b],frame[a]
    for side in 'LR':
        F=pa[side+'F']; S=pa[side+'S']; E=pa[side+'E']; W=pa[side+'W']
        R=F-pa['hip']
        v=np.gradient(R,axis=0)*FPS; s=np.linalg.norm(v,axis=1)
        ext=np.linalg.norm(F-S,axis=1)
        dh=np.linalg.norm(F-pb['head'],axis=1)
        dbody=segd(F,pb['neck'],pb['hip'])
        tgt=np.where((dh<dbody+0.08)[:,None],pb['head'],pb['neck']*0.5+pb['hip']*0.5)
        cosv=np.einsum('ij,ij->i',unit(v),unit(tgt-F))
        el=ang(S,E,W)
        last_end=0
        i=2
        while i<N-14:
            if inround[i] and np.isfinite(s[i]) and s[i]>VMIN and s[i]>=s[i-1] and s[i]>=s[i+1] and cosv[i]>0.25:
                thr=0.3*s[i]
                j=i
                while j>i-12 and j>last_end and np.isfinite(s[j-1]) and s[j-1]>thr: j-=1
                e_=i
                while e_<min(N-2,i+12) and np.isfinite(s[e_+1]) and s[e_+1]>thr and cosv[e_+1]>-0.2: e_+=1
                w=np.arange(i,min(N,max(e_+2,i+12)))
                kh=w[np.nanargmin(dh[w])] if np.isfinite(dh[w]).any() else i
                kb=w[np.nanargmin(dbody[w])] if np.isfinite(dbody[w]).any() else i
                dhm=dh[kh]; dbm=dbody[kb]
                head_target = not (dbm+0.05<dhm and F[kb,2]<pb['neck'][kb,2]-0.12)
                k=kh if head_target else kb
                k=max(k,i)
                D=R[k]-R[j]; d=fa['d'][j]; l=fa['l'][j]
                fwd=D@d; lat=D@l; up=F[k,2]-F[j,2]
                lat_in=-lat if side=='L' else lat
                stance=fa['stance'][j]
                lead_hand=(side=='L')==(stance=='O')
                elbow=el[k]
                zmax=np.nanmax(F[j:k+1,2]); shz=S[j,2]
                ok=np.isfinite([fwd,lat,up,elbow,dhm,dbm]).all() and min(dhm,dbm)<0.55 and np.linalg.norm(D)>0.15
                if ok:
                    vv=np.nanmean(v[max(j,i-2):i+3],axis=0); vv=vv/np.linalg.norm(vv)
                    fv=vv@d; lv=vv@l; uv=vv[2]; lv_in=-lv if side=='L' else lv
                    seg=R[j:k+1]; pl=np.nansum(np.linalg.norm(np.diff(seg,axis=0),axis=1)); straight=np.linalg.norm(D)/max(pl,1e-6)
                    ch=D/np.linalg.norm(D)
                    rel=seg-seg[0]; perp=rel-np.outer(rel@ch,ch)
                    m_=np.nanargmax(np.linalg.norm(perp,axis=1)); bv=perp[m_]; bn=np.linalg.norm(bv)
                    bz=bv[2]/max(bn,1e-6); bh=np.hypot(bv@d,bv@l)/max(bn,1e-6)
                    cu=ch[2]
                    ff=float(straight); fl=float(bz); fu=float(cu)
                    typ=None
                    if cu>0.75 and up>0.15: typ='UP'
                    elif straight>=0.86 or bn<0.05: typ='STR'
                    elif bz<-0.65: typ='UP' if cu>0.3 else 'SHOVEL'
                    elif bz>0.65 and not lead_hand and fwd>0.15: typ='OVER'
                    elif bh>0.75: typ='HOOK'
                    else: typ='SHOVEL' if cu>0.2 else 'HOOK'
                    if typ:
                        hit = (dhm<0.20) if head_target else (dbm<0.15)
                        punches[a].append(dict(i=i,j=j,k=k,side=side,lead=lead_hand,typ=typ,head=bool(head_target),hit=bool(hit),
                             v=float(s[i]),straight=float(straight),ff=float(ff),fl=float(fl),fu=float(fu),fwd=float(fwd),lat=float(lat_in),up=float(up),elbow=float(elbow),dh=float(dhm),db=float(dbm),stance=stance))
                last_end=max(k,e_)
                i=last_end+1
            else: i+=1
NAMES={('STR',True,True):('ジャブ','JAB'),('STR',False,True):('ストレート','STRAIGHT'),
       ('STR',True,False):('ボディジャブ','BODY JAB'),('STR',False,False):('ボディストレート','BODY STRAIGHT'),
       ('HOOK',None,True):('フック','HOOK'),('HOOK',None,False):('ボディフック','BODY HOOK'),
       ('UP',None,True):('アッパーカット','UPPERCUT'),('UP',None,False):('ボディアッパー','BODY UPPERCUT'),
       ('SHOVEL',None,True):('ショベルフック','SHOVEL HOOK'),('SHOVEL',None,False):('ショベルフック（ボディ）','SHOVEL HOOK (BODY)'),
       ('OVER',None,True):('オーバーハンド','OVERHAND'),('OVER',None,False):('オーバーハンド','OVERHAND')}
for a in (1,2):
    for p in punches[a]:
        key=(p['typ'],p['lead'] if p['typ']=='STR' else None,p['head'])
        nm,en=NAMES[key]
        if p['typ']=='HOOK' and not p['head'] and p['lead'] and ((p['stance']=='O' and p['side']=='L') or (p['stance']=='S' and p['side']=='R')):
            nm,en='レバーブロー','LIVER SHOT'
        hand='左' if p['side']=='L' else '右'
        det=f"{hand} / {'顔' if p['head'] else 'ボディ'} / {p['v']:.1f} m/s" + (' / HIT' if p['hit'] else '')
        e=add(a,t[p['j']],t[p['k']]+0.04,'PUNCH',nm,en,det,hand=p['side'],target='HEAD' if p['head'] else 'BODY',hit=bool(p['hit']),speed=round(p['v'],2))
        p['ev']=e; p['name']=nm
# combos
for a in (1,2):
    ps=sorted(punches[a],key=lambda p:p['j'])
    used=set()
    for n,p in enumerate(ps):
        if n in used or p.get('ev') is None: continue
        if p['name']=='ジャブ' and n+1<len(ps):
            q=ps[n+1]
            if q.get('ev') and q['name']=='ジャブ' and t[q['j']]-t[p['j']]<0.55:
                add(a,t[p['j']],t[q['k']]+0.04,'COMBO','ダブルジャブ','DOUBLE JAB','ジャブ×2'); used|={n,n+1}; continue
            if q.get('ev') and q['name'] in ('ストレート','ボディストレート') and t[q['j']]-t[p['j']]<0.65:
                end=q; lab=('ワンツー','ONE-TWO','ジャブ → ストレート')
                if n+2<len(ps):
                    r=ps[n+2]
                    if r.get('ev') and r['typ'] in ('HOOK','SHOVEL') and r['lead'] and t[r['j']]-t[q['j']]<0.7:
                        end=r; lab=('ワンツースリー','ONE-TWO-THREE','ジャブ → ストレート → フック'); used.add(n+2)
                add(a,t[p['j']],t[end['k']]+0.04,'COMBO',*lab); used|={n,n+1}
# counters & defense
for a,b in ((1,2),(2,1)):
    for p in punches[a]:
        if p.get('ev') is None: continue
        # counter: a's punch launched while b's punch in flight
        for q in punches[b]:
            if q.get('ev') is None: continue
            if q['j']<=p['j']<=q['k']+10 and p['hit'] and p['j']-q['j']>=2:
                cross = (not p['lead']) and q['typ']=='STR' and q['lead'] and p['typ']=='STR'
                nm,en=('クロスカウンター','CROSS COUNTER') if cross else ('カウンター','COUNTER')
                add(a,t[p['j']],t[p['k']]+0.04,'COMBO',nm,en,f"相手の{q['name']}に合わせて{p['name']}")
                break
        # defense of b
        if p['hit'] or min(p['dh'],p['db'])>0.55: continue
        pb=P[b]; fb=frame[a]
        w0=max(0,p['j']-4); w1=min(N-1,p['k']+8)
        H=pb['head'][w0:w1+1]; Hp=pb['hip'][w0:w1+1]
        if not np.isfinite(H).all(): continue
        d=fb['d'][p['j']]; l=fb['l'][p['j']]
        rel=H-H[0]; relh=(H-Hp)-(H[0]-Hp[0])
        dz=np.min(rel[:,2]); dl=rel@l; dlat=dl[np.argmax(np.abs(dl))]
        lean=np.max(relh@d)  # head moves away from attacker relative to hips (d points a->b)
        back=np.max((Hp-Hp[0])@d)
        name=None
        if dz<-0.10 and abs(dlat)>0.12: name=('ウィービング','WEAVE','上体をU字に沈めて横へ')
        elif dz<-0.12: name=('ダッキング','DUCK','真下に沈んでかわす')
        elif abs(dlat)>0.13: name=('スリッピング','SLIP','頭を左右にずらしてかわす')
        elif lean>0.09: name=('スウェーバック','SWAY BACK','上体を反らしてかわす')
        elif back>0.25: name=('バックステップ','BACK STEP','下がって距離を取る')
        else:
            # block: attacker fist met defender's forearm
            F=P[a][p['side']+'F'][p['k']]
            dd=min(np.linalg.norm(F-pb['LW'][p['k']]),np.linalg.norm(F-pb['RW'][p['k']]),np.linalg.norm(F-pb['LE'][p['k']]),np.linalg.norm(F-pb['RE'][p['k']]))
            if dd<0.14 and p['dh']<0.35: name=('ブロッキング','BLOCK','腕で受け止める')
        if name: add(b,t[w0+2],t[w1],'DEFENSE',name[0],name[1],f"{name[2]}（{p['name']}に対して）")
# clinch
dhh=np.linalg.norm((P[1]['hip']-P[2]['hip'])[:,:2],axis=1)
dhd=np.linalg.norm(P[1]['head']-P[2]['head'],axis=1)
cl=(dhh<0.55)&(dhd<0.40)&inround
cl=np.nan_to_num(sm(cl.astype(float)[:,None],11)[:,0])>0.5
i=0
while i<N:
    if cl[i]:
        j=i
        while j<N and cl[j]: j+=1
        if t[j-1]-t[i]>=0.8:
            for a in (1,2): add(a,t[i],t[j-1],'DEFENSE','クリンチ','CLINCH',f'{t[j-1]-t[i]:.1f} 秒')
        i=j
    else: i+=1
# footwork: step in / back step / side step, switch
for a,b in ((1,2),(2,1)):
    hp=P[a]['hip']; d=frame[a]['d']; l=frame[a]['l']
    W=20
    i=0
    while i<N-W:
        if not inround[i] or not np.isfinite(hp[i]).all() or not np.isfinite(hp[i+W]).all() or not np.isfinite(d[i]).all(): i+=1; continue
        mv=hp[i+W]-hp[i]; f=mv@d[i]; s_=mv@l[i]
        nm=None
        if f>0.45: nm=('ステップイン','STEP IN','踏み込んで距離を詰める')
        elif f<-0.45: nm=('バックステップ','BACK STEP','下がって距離を取る')
        elif abs(s_)>0.6 and abs(f)<0.2: nm=('サイドステップ','SIDE STEP','横に動いて正面を外す')
        if nm:
            add(a,t[i],t[i+W],'FOOTWORK',*nm); i+=W+10
        else: i+=2
    st=frame[a]['stance']; ld=frame[a]['lead']
    prev=None; runs=[]
    i=0
    while i<N:
        if inround[i] and np.isfinite(ld[i]):
            cur=st[i]
            if prev is not None and cur!=prev[0]:
                # require new stance held >=1.5 s
                k=i
                while k<N and (not np.isfinite(ld[k]) or st[k]==cur): k+=1
                if t[k-1]-t[i]>=3.0:
                    add(a,t[i]-0.3,t[i]+0.5,'FOOTWORK','スイッチ','SWITCH','オーソドックス → サウスポー' if cur=='S' else 'サウスポー → オーソドックス')
                    prev=(cur,i)
                i=k; continue
            if prev is None: prev=(cur,i)
        i+=1
CL=[(e['t0'],e['t1']) for e in events if e['en']=='CLINCH' and e['jersey']==1]
def in_cl(x): return any(a-0.2<=x<=b+0.2 for a,b in CL)
events=[e for e in events if e['en']=='CLINCH' or e['kind']=='FOOTWORK' or not in_cl(e['t0'])]
events.sort(key=lambda e:(e['t0'],e['jersey']))
json.dump(events,open('techniques_F9.json','w'),ensure_ascii=False,indent=0)
import collections
c=collections.Counter((e['corner'],e['name']) for e in events)
for k,v in sorted(c.items()): print(k,v)
print('total',len(events))
for a in (1,2):
    st=frame[a]['stance'][inround]; print(a,'orthodox ratio',np.mean(st=='O'))
