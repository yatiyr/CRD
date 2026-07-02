#!/usr/bin/env python3
# Faithful port of Ruckig's PositionThirdOrderStep1 (min-time, arbitrary state), using my own integrate+validate as
# the profile 'check'. If this matches ruckig 200/200 in python, the C++ port is a direct transcription.
import math
import numpy as np
from ruckig import Ruckig, InputParameter, Trajectory

EPS = 2.220446049250313e-16

def check(t, jMax, p0, v0, a0, pf, vf, af, vmax, amax, tol=1e-8):
    # Faithful transcription of Ruckig Profile::check<UDDU> (symmetric limits). Velocity is enforced only on the BACK
    # half (v[3..6]) + interior a=0 crossings for phases>1 -> the front-half overshoot from a hot initial state is
    # allowed (physically unavoidable when |v0|,|a0| are large); accel stays hard at a[1],a[3],a[5].
    if min(t) < -1e-12:
        return None
    js = [jMax if t[0] > 0 else 0.0, 0.0, -jMax if t[2] > 0 else 0.0, 0.0,
          -jMax if t[4] > 0 else 0.0, 0.0, jMax if t[6] > 0 else 0.0]
    va = [0.0]*8; aa = [0.0]*8; pa = [0.0]*8
    pa[0], va[0], aa[0] = p0, v0, a0
    for i in range(7):
        dur = max(t[i], 0.0)
        aa[i+1] = aa[i] + dur*js[i]
        va[i+1] = va[i] + dur*(aa[i] + dur*js[i]/2)
        pa[i+1] = pa[i] + dur*(va[i] + dur*(aa[i]/2 + dur*js[i]/6))
    veps = 1e-7; aeps = 1e-7
    if abs(pa[7]-pf) > 1e-6 or abs(va[7]-vf) > 1e-6 or abs(aa[7]-af) > 1e-6:
        return None
    for idx in (1, 3, 5):
        if aa[idx] > amax+aeps or aa[idx] < -amax-aeps: return None
    for idx in (3, 4, 5, 6):
        if va[idx] > vmax+veps or va[idx] < -vmax-veps: return None
    for i in range(2, 7):
        if aa[i+1]*aa[i] < -1e-16 and abs(js[i]) > 1e-15:
            v_a_zero = va[i] - aa[i]*aa[i]/(2*js[i])
            if v_a_zero > vmax+veps or v_a_zero < -vmax-veps: return None
    return sum(max(x, 0.0) for x in t)

def qroots(poly4):  # monic x^4 + a x^3 + b x^2 + c x + d
    a, b, c, d = poly4
    r = np.roots([1.0, a, b, c, d])
    return [x.real for x in r if abs(x.imag) < 1e-9]

def step1(p0, v0, a0, pf, vf, af, VMAX, AMAX, JMAX):
    pd0 = pf - p0
    v0_v0=v0*v0; vf_vf=vf*vf; a0_a0=a0*a0; af_af=af*af
    a0_p3=a0*a0_a0; a0_p4=a0_a0*a0_a0; af_p3=af*af_af; af_p4=af_af*af_af
    cands = []  # list of profile t-arrays (each tried under its jMax)

    def add(t, jMax):
        cands.append((list(t), jMax))

    for (vMax, vMin, aMax, aMin, jMax) in [(VMAX,-VMAX,AMAX,-AMAX,JMAX), (-VMAX,VMAX,-AMAX,AMAX,-JMAX)]:
        jj = jMax*jMax; pd = pd0
        # ---- time_all_vel: ACC0_ACC1_VEL, ACC1_VEL, ACC0_VEL, VEL ----
        t = [0]*7
        t[0]=(-a0+aMax)/jMax; t[1]=(a0_a0/2 - aMax*aMax - jMax*(v0-vMax))/(aMax*jMax); t[2]=aMax/jMax
        t[3]=(3*(a0_p4*aMin - af_p4*aMax) + 8*aMax*aMin*(af_p3 - a0_p3 + 3*jMax*(a0*v0 - af*vf)) + 6*a0_a0*aMin*(aMax*aMax - 2*jMax*v0) - 6*af_af*aMax*(aMin*aMin - 2*jMax*vf) - 12*jMax*(aMax*aMin*(aMax*(v0+vMax) - aMin*(vf+vMax) - 2*jMax*pd) + (aMin-aMax)*jMax*vMax*vMax + jMax*(aMax*vf_vf - aMin*v0_v0)))/(24*aMax*aMin*jj*vMax)
        t[4]=-aMin/jMax; t[5]=-(af_af/2 - aMin*aMin - jMax*(vf-vMax))/(aMin*jMax); t[6]=t[4]+af/jMax
        add(t, jMax)
        disc0 = a0_a0/(2*jj) + (vMax - v0)/jMax
        if disc0 >= 0:
            t_acc0 = math.sqrt(disc0)
            t=[0]*7; t[0]=t_acc0 - a0/jMax; t[1]=0; t[2]=t_acc0
            t[3]=-(3*af_p4 - 8*aMin*(af_p3 - a0_p3) - 24*aMin*jMax*(a0*v0 - af*vf) + 6*af_af*(aMin*aMin - 2*jMax*vf) - 12*jMax*(2*aMin*jMax*pd + aMin*aMin*(vf+vMax) + jMax*(vMax*vMax - vf_vf) + aMin*t_acc0*(a0_a0 - 2*jMax*(v0+vMax))))/(24*aMin*jj*vMax)
            t[4]=-aMin/jMax; t[5]=-(af_af/2 - aMin*aMin + jMax*(vMax - vf))/(aMin*jMax); t[6]=t[4]+af/jMax
            add(t, jMax)
        disc1 = af_af/(2*jj) + (vMax - vf)/jMax
        if disc1 >= 0:
            t_acc1 = math.sqrt(disc1)
            t=[0]*7; t[0]=(-a0+aMax)/jMax; t[1]=(a0_a0/2 - aMax*aMax - jMax*(v0-vMax))/(aMax*jMax); t[2]=aMax/jMax
            t[3]=(3*a0_p4 + 8*aMax*(af_p3 - a0_p3) + 24*aMax*jMax*(a0*v0 - af*vf) + 6*a0_a0*(aMax*aMax - 2*jMax*v0) - 12*jMax*(-2*aMax*jMax*pd + aMax*aMax*(v0+vMax) + jMax*(vMax*vMax - v0_v0) + aMax*t_acc1*(-af_af + 2*(vf+vMax)*jMax)))/(24*aMax*jj*vMax)
            t[4]=t_acc1; t[5]=0; t[6]=t_acc1 + af/jMax
            add(t, jMax)
        if disc0 >= 0 and disc1 >= 0:
            t_acc0=math.sqrt(disc0); t_acc1=math.sqrt(disc1)
            t=[0]*7; t[0]=t_acc0 - a0/jMax; t[1]=0; t[2]=t_acc0
            t[3]=(af_p3 - a0_p3)/(3*jj*vMax) + (a0*v0 - af*vf + (af_af*t_acc1 + a0_a0*t_acc0)/2)/(jMax*vMax) - (v0/vMax + 1.0)*t_acc0 - (vf/vMax + 1.0)*t_acc1 + pd/vMax
            t[4]=t_acc1; t[5]=0; t[6]=t_acc1 + af/jMax
            add(t, jMax)
        # ---- time_acc0_acc1 ----
        h1 = (3*(af_p4*aMax - a0_p4*aMin) + aMax*aMin*(8*(a0_p3 - af_p3) + 3*aMax*aMin*(aMax - aMin) + 6*aMin*af_af - 6*aMax*a0_a0) + 12*jMax*(aMax*aMin*((aMax - 2*a0)*v0 - (aMin - 2*af)*vf) + aMin*a0_a0*v0 - aMax*af_af*vf))/(3*(aMax - aMin)*jj) + 4*(aMax*vf_vf - aMin*v0_v0 - 2*aMin*aMax*pd)/(aMax - aMin)
        if h1 >= 0:
            h1 = math.sqrt(h1)/2
            h2 = a0_a0/(2*aMax*jMax) + (aMin - 2*aMax)/(2*jMax) - v0/aMax
            h3 = -af_af/(2*aMin*jMax) - (aMax - 2*aMin)/(2*jMax) + vf/aMin
            for sol in (+1,-1):
                t=[0]*7; t[0]=(-a0+aMax)/jMax; t[1]=h2 - sol*h1/aMax; t[2]=aMax/jMax; t[3]=0
                t[4]=-aMin/jMax; t[5]=h3 + sol*h1/aMin; t[6]=t[4]+af/jMax
                add(t, jMax)
        # ---- time_all_none_acc0_acc1 ----
        h2_none = (a0_a0 - af_af)/(2*jMax) + (vf - v0); h2_h2 = h2_none*h2_none
        t_min_none=(a0-af)/jMax; t_max_none=(aMax-aMin)/jMax
        poly_none=[-2*(a0_a0+af_af-2*jMax*(v0+vf))/jj, 4*(a0_p3-af_p3+3*jMax*(af*vf-a0*v0))/(3*jMax*jj)-4*pd/jMax, -h2_h2/jj]
        h3_acc0=(a0_a0-af_af)/(2*aMax*jMax)+(vf-v0)/aMax; t_min_acc0=(aMax-af)/jMax; t_max_acc0=(aMax-aMin)/jMax
        h0_acc0=3*(af_p4-a0_p4)+8*(a0_p3-af_p3)*aMax+24*aMax*jMax*(af*vf-a0*v0)-6*a0_a0*(aMax*aMax-2*jMax*v0)+6*af_af*(aMax*aMax-2*jMax*vf)+12*jMax*(jMax*(vf_vf-v0_v0-2*aMax*pd)-aMax*aMax*(vf-v0))
        h2_acc0=-af_af+aMax*aMax+2*jMax*vf
        poly_acc0=[h2_acc0/jj, 0.0, h0_acc0/(12*jj*jj)]
        h3_acc1=-(a0_a0+af_af)/(2*jMax*aMin)+aMin/jMax+(vf-v0)/aMin; t_min_acc1=(aMin-a0)/jMax; t_max_acc1=(aMax-a0)/jMax
        h0_acc1=(a0_p4-af_p4)/4+2*(af_p3-a0_p3)*aMin/3+(a0_a0-af_af)*aMin*aMin/2+jMax*(af_af*vf+a0_a0*v0+2*aMin*(jMax*pd-a0*v0-af*vf)+aMin*aMin*(v0+vf)+jMax*(v0_v0-vf_vf))
        h2_acc1=a0_a0-a0*aMin+2*jMax*v0
        poly_acc1=[(5*a0_a0+aMin*(aMin-6*a0)+2*jMax*v0)/jj, 2*(a0-aMin)*h2_acc1/(jj*jMax), h0_acc1/(jj*jj)]
        # roots (leading coeff for none is 0 -> quartic with a=coeff? Ruckig uses solve_quart_monic on [0,b,c,d]; the
        # array is [t^3 coeff, t^2, t^1, t^0] with implied t^4 monic). Build monic quartics:
        for (poly, tmn, tmx, kind) in [ (poly_none, t_min_none, t_max_none, 'none'),
                                        (poly_acc0, t_min_acc0, t_max_acc0, 'acc0'),
                                        (poly_acc1, t_min_acc1, t_max_acc1, 'acc1') ]:
            a4 = [ (0.0 if kind=='none' else (poly_acc0[0] if kind=='acc0' else poly_acc1[0])), 0,0,0 ]
            # reconstruct the monic quartic coefficients [a,b,c,d]:
            if kind=='none':
                A=0.0; B=poly_none[0]; C=poly_none[1]; D=poly_none[2]
            elif kind=='acc0':
                A=-2*aMax/jMax; B=poly_acc0[0]; C=0.0; D=poly_acc0[2]
            else:
                A=2*(2*a0-aMin)/jMax; B=poly_acc1[0]; C=poly_acc1[1]; D=poly_acc1[2]
            for t in qroots([A,B,C,D]):
                if t < tmn - 1e-9 or t > tmx + 1e-9: continue
                if kind=='none':
                    h0=h2_none/(2*jMax*t) if abs(t)>1e-12 else 0.0
                    tt=[0]*7; tt[0]=h0+t/2 - a0/jMax; tt[2]=t; tt[6]=-h0+t/2+af/jMax
                elif kind=='acc0':
                    tt=[0]*7; tt[0]=(-a0+aMax)/jMax; tt[1]=h3_acc0 - 2*t + jMax/aMax*t*t; tt[2]=t; tt[6]=(af-aMax)/jMax + t
                else:
                    tt=[0]*7; tt[0]=t; tt[2]=(a0-aMin)/jMax + t; tt[5]=h3_acc1 - (2*a0+jMax*t)*t/aMin; tt[6]=(af-aMin)/jMax
                add(tt, jMax)
        # ---- two-step degenerate fallbacks ----
        # none_two_step
        val=(a0_a0+af_af)/2 + jMax*(vf-v0)
        if val>=0:
            h0=math.sqrt(val)*(abs(jMax)/jMax)
            t=[0]*7; t[0]=(h0-a0)/jMax; t[2]=(h0-af)/jMax; add(t,jMax)
        t=[0]*7; t[0]=(af-a0)/jMax; add(t,jMax)
        # acc0_two_step
        if abs(a0)>1e-12:
            t=[0]*7; t[1]=(af_af-a0_a0+2*jMax*(vf-v0))/(2*a0*jMax); t[2]=(a0-af)/jMax; add(t,jMax)
        t=[0]*7; t[0]=(-a0+aMax)/jMax; t[1]=(a0_a0+af_af-2*aMax*aMax+2*jMax*(vf-v0))/(2*aMax*jMax); t[2]=(-af+aMax)/jMax; add(t,jMax)
        # vel_two_step
        if disc1>=0:
            h1v=math.sqrt(disc1)
            t=[0]*7; t[0]=-a0/jMax; t[3]=(af_p3-a0_p3)/(3*jj*vMax)+(a0*v0-af*vf+(af_af*h1v)/2)/(jMax*vMax)-(vf/vMax+1.0)*h1v+pd/vMax; t[4]=h1v; t[6]=h1v+af/jMax; add(t,jMax)
            t=[0]*7; t[2]=a0/jMax; t[3]=(af_p3-a0_p3)/(3*jj*vMax)+(a0*v0-af*vf+(af_af*h1v+a0_p3/jMax)/2)/(jMax*vMax)-(v0/vMax+1.0)*a0/jMax-(vf/vMax+1.0)*h1v+pd/vMax; t[4]=h1v; t[6]=h1v+af/jMax; add(t,jMax)

    # evaluate all candidates
    best=None
    for (t, jMax) in cands:
        T = check(t, jMax, p0, v0, a0, pf, vf, af, VMAX, AMAX)
        if T is not None and (best is None or T < best[0]-1e-9):
            best=(T, t, jMax)
    return best

def v_at_t(v0,a0,j,t): return v0 + t*(a0 + j*t/2)
def v_at_a_zero(v0,a0,j): return v0 + a0*a0/(2*j)

def _vel_brake(v0,a0,vMax,vMin,aMax,aMin,jMax,bt,bj):
    bj[0]=-jMax
    t_to_a_min=(a0-aMin)/jMax
    arg1=a0*a0+2*jMax*(v0-vMax); arg2=a0*a0/2+jMax*(v0-vMin)
    t_to_v_max=a0/jMax + math.sqrt(max(arg1,0.0))/abs(jMax)
    t_to_v_min=a0/jMax + math.sqrt(max(arg2,0.0))/abs(jMax)
    t_min_to_v_max=min(t_to_v_max,t_to_v_min)
    if t_to_a_min < t_min_to_v_max:
        v_at_a_min=v_at_t(v0,a0,-jMax,t_to_a_min)
        t_to_v_max_c=-(v_at_a_min-vMax)/aMin
        t_to_v_min_c=aMin/(2*jMax)-(v_at_a_min-vMin)/aMin
        bt[0]=max(t_to_a_min,0.0); bt[1]=max(min(t_to_v_max_c,t_to_v_min_c),0.0)
    else:
        bt[0]=max(t_min_to_v_max,0.0)

def _accel_brake(v0,a0,vMax,vMin,aMax,aMin,jMax,bt,bj):
    bj[0]=-jMax
    t_to_a_max=(a0-aMax)/jMax; t_to_a_zero=a0/jMax
    v_am=v_at_t(v0,a0,-jMax,t_to_a_max); v_az=v_at_t(v0,a0,-jMax,t_to_a_zero)
    if (v_az>vMax and jMax>0) or (v_az<vMax and jMax<0):
        _vel_brake(v0,a0,vMax,vMin,aMax,aMin,jMax,bt,bj)
    elif (v_am<vMin and jMax>0) or (v_am>vMin and jMax<0):
        t_to_v_min=-(v_am-vMin)/aMax; t_to_v_max=-aMax/(2*jMax)-(v_am-vMax)/aMax
        bt[0]=t_to_a_max; bt[1]=max(min(t_to_v_min,t_to_v_max),0.0)
    else:
        bt[0]=t_to_a_max

def brake_traj(v0,a0,VMAX,AMAX,JMAX):
    vMax,vMin,aMax,aMin,jMax=VMAX,-VMAX,AMAX,-AMAX,JMAX
    bt=[0.0,0.0]; bj=[0.0,0.0]
    if jMax==0 or aMax==0 or aMin==0: return bt,bj
    if a0 > aMax:
        _accel_brake(v0,a0,vMax,vMin,aMax,aMin,jMax,bt,bj)
    elif a0 < aMin:
        _accel_brake(v0,a0,vMin,vMax,aMin,aMax,-jMax,bt,bj)
    elif (v0>vMax and v_at_a_zero(v0,a0,-jMax)>vMin) or (a0>0 and v_at_a_zero(v0,a0,jMax)>vMax):
        _vel_brake(v0,a0,vMax,vMin,aMax,aMin,jMax,bt,bj)
    elif (v0<vMin and v_at_a_zero(v0,a0,jMax)<vMax) or (a0<0 and v_at_a_zero(v0,a0,-jMax)<vMin):
        _vel_brake(v0,a0,vMin,vMax,aMin,aMax,-jMax,bt,bj)
    return bt,bj

def solve_full(p0,v0,a0,pf,vf,af,vmax,amax,jmax):
    bt,bj=brake_traj(v0,a0,vmax,amax,jmax)
    # advance through brake phases
    p,v,a=p0,v0,a0
    for dur,j in zip(bt,bj):
        p=p+v*dur+0.5*a*dur*dur+j*dur*dur*dur/6.0; v=v+a*dur+0.5*j*dur*dur; a=a+j*du
    r=step1(p,v,a,pf,vf,af,vmax,amax,jmax)
    if r is None: return None
    return (bt[0]+bt[1]+r[0], bt, bj, r[1], r[2])

def ruckig_dur(p0,v0,a0,pf,vf,af,vmax,amax,jmax):
    inp=InputParameter(1); inp.current_position=[p0]; inp.current_velocity=[v0]; inp.current_acceleration=[a0]
    inp.target_position=[pf]; inp.target_velocity=[vf]; inp.target_acceleration=[af]
    inp.max_velocity=[vmax]; inp.max_acceleration=[amax]; inp.max_jerk=[jmax]
    tr=Trajectory(1); Ruckig(1).calculate(inp,tr); return tr.duration

rng=np.random.RandomState(7); npass=nfail=0; misses=[]; nskip=0
for it in range(2000):
    vmax=rng.uniform(0.3,4); amax=rng.uniform(0.3,3); jmax=rng.uniform(0.5,5)
    v0=rng.uniform(-1.1,1.1)*vmax; a0=rng.uniform(-1.1,1.1)*amax
    vf=rng.uniform(-0.9,0.9)*vmax; af=rng.uniform(-0.7,0.7)*amax; pf=rng.uniform(-25,25)
    try:
        rd=ruckig_dur(0,v0,a0,pf,vf,af,vmax,amax,jmax)
    except Exception:
        nskip+=1; continue
    r=solve_full(0,v0,a0,pf,vf,af,vmax,amax,jmax)
    if r is not None and abs(r[0]-rd)<1e-4: npass+=1
    else:
        nfail+=1
        if len(misses)<8: misses.append(((round(v0,3),round(a0,3),round(vf,3),round(af,3),round(pf,2),round(vmax,2),round(amax,2),round(jmax,2)),round(rd,4),None if r is None else round(r[0],4)))
print(f"faithful step1+brake vs ruckig: {npass}/{npass+nfail} match ({nskip} infeasible targets skipped)")
for m in misses: print("  miss:", m)
