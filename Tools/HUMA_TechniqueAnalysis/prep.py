import numpy as np
z=np.load('f9_people.npz'); T=z['T']; N=len(T)
t=(T-T[0])/1e6
out={'t':t}
for jn in (1,2):
    m=(z['role']==1)&(z['jersey']==jn)
    J=np.full((N,29,3),np.nan,np.float32)
    J[z['frame'][m]]=z['J'][m]
    out[f'J{jn}']=J
    print(jn, m.sum(), np.isnan(J[:,0,0]).sum())
np.savez_compressed('f9_players.npz',**out)
print(np.diff(t).min(),np.diff(t).max(),np.median(np.diff(t)))
