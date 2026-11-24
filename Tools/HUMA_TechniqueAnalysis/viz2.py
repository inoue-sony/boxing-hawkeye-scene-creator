import numpy as np, json, sys, matplotlib
matplotlib.use('Agg'); import matplotlib.pyplot as plt
z=np.load('f9_players.npz'); t=z['t']; J={1:z['J1'],2:z['J2']}
E=json.load(open('techniques_F9.json'))
C=[(1,8),(1,2),(1,5),(2,3),(3,4),(5,6),(6,7),(8,9),(9,10),(10,11),(8,12),(12,13),(13,14),(1,0)]
def draw(ax,P,col,alpha,proj,lw=1.5):
    for a,b in C:
        p=proj(P[[a,b]]); ax.plot(p[:,0],p[:,1],color=col,alpha=alpha,lw=lw)
    h=proj(np.nanmean(P[[0,15,16,17,18]],0)[None])[0]; ax.add_patch(plt.Circle(h,0.1,fill=False,color=col,alpha=alpha))
def show(evs,fn):
    n=len(evs); fig,axs=plt.subplots(2,n,figsize=(3.2*n,6.4))
    for c,e in enumerate(evs):
        i0=np.searchsorted(t,e['t0']); i1=np.searchsorted(t,e['t1']-0.04)
        a=e['jersey']; b=3-a
        d=J[b][i0,8]-J[a][i0,8]; d[2]=0; d/=np.linalg.norm(d); l=np.cross([0,0,1],d); o=J[a][i0,8].copy()
        side=lambda P:np.stack([(P-o)@d,P[:,2]],1); top=lambda P:np.stack([(P-o)@d,(P-o)@l],1)
        for r,proj in enumerate((side,top)):
            ax=axs[r,c]
            draw(ax,J[a][i0],'gray',0.6,proj); draw(ax,J[a][i1],'r' if a==1 else 'b',1,proj)
            draw(ax,J[b][i1],'b' if a==1 else 'r',0.35,proj)
            idx=[7,25,26] if e['hand']=='L' else [4,27,28]
            F=np.nanmean(J[a][i0:i1+1][:,idx],1); p=proj(F); ax.plot(p[:,0],p[:,1],'g-',lw=2); ax.plot(p[-1,0],p[-1,1],'g>',ms=8)
            ax.set_aspect('equal'); ax.set_xlim(-0.6,1.6)
            if r==0: ax.set_ylim(0,2)
            else: ax.set_ylim(-0.8,0.8)
            ax.set_title(f"{e['corner'][0]} {e['en']} {e['t0']:.2f}\n{e['detail']}",fontsize=7)
    plt.tight_layout(); plt.savefig(fn,dpi=65)
filt=sys.argv[1]; corner=sys.argv[2]; hand=sys.argv[3]; seed=int(sys.argv[5])
c=[e for e in E if e['en']==filt and e['corner']==corner and e.get('hand')==hand]
rng=np.random.default_rng(seed); sel=[c[i] for i in rng.choice(len(c),min(6,len(c)),replace=False)]
show(sel,sys.argv[4])
