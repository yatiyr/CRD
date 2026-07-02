#!/usr/bin/env python3
# Reconstruct Ruckig's PositionThirdOrderStep2 (reach the target in EXACTLY tf), for multi-DoF time-synchronization.
# Uses numpy.roots for the polynomial solving (ruckig uses deterministic derivative-bracketing; the roots are the same)
# + a faithful check_with_timing. Verified against the ruckig package's multi-DoF sync (the slow DoF IS step2(tsync)).
import math
import numpy as np
from ruckig import Ruckig, InputParameter, Trajectory

EPS = 2.220446049250313e-16

def check_wt(t, ctrl, jMax, p0, v0, a0, pf, vf, af, vmax, amax, tf):
    # ctrl: 'UDDU' -> j=[+,-,-,+] pattern (phases 0,2,4,6); 'UDUD' -> [+,-,+,-]
    for x in t:
        if x < -1e-10 or math.isnan(x): return None
    if ctrl == 'UDDU':
        js = [jMax if t[0]>0 else 0,0, -jMax if t[2]>0 else 0,0, -jMax if t[4]>0 else 0,0, jMax if t[6]>0 else 0]
    else:
        js = [jMax if t[0]>0 else 0,0, -jMax if t[2]>0 else 0,0, jMax if t[4]>0 else 0,0, -jMax if t[6]>0 else 0]
    va=[0.0]*8; aa=[0.0]*8; pa=[0.0]*8; pa[0],va[0],aa[0]=p0,v0,a0; total=0.0
    for i in range(7):
        d=max(t[i],0.0); total+=d
        aa[i+1]=aa[i]+d*js[i]; va[i+1]=va[i]+d*(aa[i]+d*js[i]/2); pa[i+1]=pa[i]+d*(va[i]+d*(aa[i]/2+d*js[i]/6))
    if abs(total-tf)>1e-6: return None
    if abs(pa[7]-pf)>1e-6 or abs(va[7]-vf)>1e-6 or abs(aa[7]-af)>1e-6: return None
    ve=1e-6; ae=1e-6
    for idx in (1,3,5):
        if aa[idx]>amax+ae or aa[idx]<-amax-ae: return None
    for idx in (3,4,5,6):
        if va[idx]>vmax+ve or va[idx]<-vmax-ve: return None
    for i in range(2,7):
        if aa[i+1]*aa[i]<-1e-16 and abs(js[i])>1e-15:
            vaz=va[i]-aa[i]*aa[i]/(2*js[i])
            if vaz>vmax+ve or vaz<-vmax-ve: return None
    return total

def qroots(poly):  # poly = full coeff list high->low (leading may be !=1); returns real roots
    r=np.roots(poly); return [x.real for x in r if abs(x.imag)<1e-7]

def step2(p0,v0,a0,pf,vf,af,VMAX,AMAX,JMAX,tf):
    pd0=pf-p0; vd=vf-v0; vd_vd=vd*vd; v0_v0=v0*v0; vf_vf=vf*vf; ad=af-a0; ad_ad=ad*ad
    a0_a0=a0*a0; af_af=af*af; a0_p3=a0*a0_a0; a0_p4=a0_a0*a0_a0; a0_p5=a0_p3*a0_a0; a0_p6=a0_p4*a0_a0
    af_p3=af*af_af; af_p4=af_af*af_af; af_p5=af_p3*af_af; af_p6=af_p4*af_af
    tf_tf=tf*tf; tf_p3=tf_tf*tf; tf_p4=tf_tf*tf_tf
    best=None
    def consider(t, ctrl, jf):
        nonlocal best
        d=check_wt(t,ctrl,jf,p0,v0,a0,pf,vf,af,VMAX,AMAX,tf)
        if d is not None and best is None:
            best=(list(t),ctrl,jf)
    for (vMax,vMin,aMax,aMin,jMax) in [(VMAX,-VMAX,AMAX,-AMAX,JMAX),(-VMAX,VMAX,-AMAX,AMAX,-JMAX)]:
        jj=jMax*jMax; pd=pd0; g1=-pd+tf*v0; g2=-2*pd+tf*(v0+vf)
        # ===== time_acc0_acc1_vel =====
        if (2*(aMax-aMin)+ad)/jMax < tf:
            inner=(a0_p4+af_p4-4*a0_p3*(2*aMax+aMin)/3-4*af_p3*(aMax+2*aMin)/3+2*(a0_a0-af_af)*aMax*aMax+(4*a0*aMax-2*a0_a0)*(af_af-2*af*aMin+(aMin-aMax)*aMin+2*jMax*(aMin*tf-vd))+2*af_af*(aMin*aMin+2*jMax*(aMax*tf-vd))+4*jMax*(2*aMin*(af*vd+jMax*g1)+(aMax*aMax-aMin*aMin)*vd+jMax*vd_vd)+8*aMax*jj*(pd-tf*vf))/(aMax*aMin)+4*af_af+2*a0_a0+(4*af+aMax-aMin)*(aMax-aMin)+4*jMax*(aMin-aMax+jMax*tf-2*af)*tf
            if inner>=0:
                h1=math.sqrt(inner)*(abs(jMax)/jMax)
                t=[0]*7
                t[0]=(-a0+aMax)/jMax
                t[1]=(-(af_af-a0_a0+2*aMax*aMax+aMin*(aMin-2*ad-3*aMax)+2*jMax*(aMin*tf-vd))+aMin*h1)/(2*(aMax-aMin)*jMax)
                t[2]=aMax/jMax; t[3]=(aMin-aMax+h1)/(2*jMax); t[4]=-aMin/jMax
                t[5]=tf-(t[0]+t[1]+t[2]+t[3]+2*t[4]+af/jMax); t[6]=t[4]+af/jMax
                consider(t,'UDDU',jMax)
        if (-a0+4*aMax-af)/jMax < tf:
            den=(a0_a0+af_af-2*(a0+af)*aMax+2*(aMax*aMax-aMax*jMax*tf+jMax*vd))
            if abs(den)>1e-14:
                t=[0]*7; t[0]=(-a0+aMax)/jMax
                t[1]=(3*(a0_p4+af_p4)-4*(a0_p3+af_p3)*aMax-4*af_p3*aMax+24*(a0+af)*aMax*aMax*aMax-6*(af_af+a0_a0)*(aMax*aMax-2*jMax*vd)+6*a0_a0*(af_af-2*af*aMax-2*aMax*jMax*tf)-12*aMax*aMax*(2*aMax*aMax-2*aMax*jMax*tf+jMax*vd)-24*af*aMax*jMax*vd+12*jj*(2*aMax*g1+vd_vd))/(12*aMax*jMax*den)
                t[2]=aMax/jMax; t[3]=(-a0_a0-af_af+2*aMax*(a0+af-2*aMax)-2*jMax*vd)/(2*aMax*jMax)+tf
                t[4]=t[2]; t[5]=tf-(t[0]+t[1]+t[2]+t[3]+2*t[4]-af/jMax); t[6]=t[4]-af/jMax
                consider(t,'UDUD',jMax)
        # ===== time_acc1_vel (UDDU) =====
        ph1=a0_a0+af_af-aMin*(a0+2*af-aMin)-2*jMax*(vd-aMin*tf); ph2=2*aMin*(jMax*g1+af*vd)-aMin*aMin*vd+jMax*vd_vd; ph3=af_af+aMin*(aMin-2*af)-2*jMax*(vd-aMin*tf)
        P=[jj*jj, (2*(2*a0-aMin))/jMax*jj*jj/jj, 0,0,0]  # placeholder not used
        poly=[1.0,(2*(2*a0-aMin))/jMax,(4*a0_a0+ph1-3*a0*aMin)/jj,(2*a0*ph1)/(jj*jMax),(3*(a0_p4+af_p4)-4*(a0_p3+2*af_p3)*aMin+6*af_af*(aMin*aMin-2*jMax*vd)+12*jMax*ph2+6*a0_a0*ph3)/(12*jj*jj)]
        for t0 in qroots(poly):
            h1=-((a0_a0+af_af)/2+jMax*(-vd+2*a0*t0+jMax*t0*t0))/aMin
            t=[0]*7; t[0]=t0; t[2]=a0/jMax+t0; t[3]=tf-(h1-aMin+a0+af)/jMax-2*t0; t[4]=-aMin/jMax; t[5]=(h1+aMin)/jMax; t[6]=t[4]+af/jMax
            consider(t,'UDDU',jMax)
        # ===== time_acc1_vel (UDUD) =====
        ph1=a0_a0-af_af+(2*af-a0)*aMax-aMax*aMax-2*jMax*(vd-aMax*tf); ph2=aMax*aMax+2*jMax*vd; ph3=af_af+ph2-2*aMax*(af+jMax*tf); ph4=2*aMax*jMax*g1+aMax*aMax*vd+jMax*vd_vd
        poly=[1.0,(4*a0-2*aMax)/jMax,(4*a0_a0-3*a0*aMax+ph1)/jj,(2*a0*ph1)/(jj*jMax),(3*(a0_p4+af_p4)-4*(a0_p3+2*af_p3)*aMax-24*af*aMax*jMax*vd+12*jMax*ph4-6*a0_a0*ph3+6*af_af*ph2)/(12*jj*jj)]
        for t0 in qroots(poly):
            h1=((a0_a0-af_af)/2+jj*t0*t0-jMax*(vd-2*a0*t0))/aMax
            t=[0]*7; t[0]=t0; t[2]=t0+a0/jMax; t[3]=tf+(h1+ad-aMax)/jMax-2*t0; t[4]=aMax/jMax; t[5]=-(h1+aMax)/jMax; t[6]=t[4]-af/jMax
            consider(t,'UDUD',jMax)
        # ===== time_acc0_vel =====
        ph1x=12*jMax*(-aMax*aMax*vd-jMax*vd_vd+2*aMax*jMax*(-pd+tf*vf))
        poly=[1.0,(2*aMax)/jMax,(a0_a0-af_af+2*ad*aMax+aMax*aMax+2*jMax*(vd-aMax*tf))/jj,0.0,-(-3*(a0_p4+af_p4)+4*(af_p3+2*a0_p3)*aMax-12*a0*aMax*(af_af-2*jMax*vd)+6*a0_a0*(af_af-aMax*aMax-2*jMax*vd)+6*af_af*(aMax*aMax-2*aMax*jMax*tf+2*jMax*vd)+ph1x)/(12*jj*jj)]
        for t0 in qroots(poly):
            h1=((a0_a0-af_af)/2+jMax*(jMax*t0*t0+vd))/aMax
            t=[0]*7; t[0]=(-a0+aMax)/jMax; t[1]=(h1-aMax)/jMax; t[2]=aMax/jMax; t[3]=tf-(h1+ad+aMax)/jMax-2*t0; t[4]=t0; t[6]=af/jMax+t0
            consider(t,'UDDU',jMax)
        poly=[1.0,(-2*aMax)/jMax,-(a0_a0+af_af-2*(a0+af)*aMax+aMax*aMax+2*jMax*(vd-aMax*tf))/jj,0.0,(3*(a0_p4+af_p4)-4*(af_p3+2*a0_p3)*aMax+6*a0_a0*(af_af+aMax*aMax+2*jMax*vd)-12*a0*aMax*(af_af+2*jMax*vd)+6*af_af*(aMax*aMax-2*aMax*jMax*tf+2*jMax*vd)-ph1x)/(12*jj*jj)]
        for t0 in qroots(poly):
            h1=((a0_a0+af_af)/2+jMax*(vd-jMax*t0*t0))/aMax
            t=[0]*7; t[0]=(-a0+aMax)/jMax; t[1]=(h1-aMax)/jMax; t[2]=aMax/jMax; t[3]=tf-(h1-a0-af+aMax)/jMax-2*t0; t[4]=t0; t[6]=-(af/jMax)+t0
            consider(t,'UDUD',jMax)
        # ===== time_vel (UDDU general via 5th-order numpy roots) =====
        if not (abs(v0)<EPS and abs(a0)<EPS and abs(vf)<EPS and abs(af)<EPS):
            p1=af_af-2*jMax*(-2*af*tf+jMax*tf_tf+3*vd); ph1=af_p3-3*jj*g1-3*af*jMax*vd
            ph2=af_p4+8*af_p3*jMax*tf+12*jMax*(3*jMax*vd_vd-af_af*vd+2*af*jMax*(g1-tf*vd)-2*jj*tf*g1)
            ph3=a0*(af-jMax*tf); ph4=jMax*(-ad+jMax*tf)
            if abs(ph4)>1e-14:
                poly=[1.0,(15*a0_a0+af_af+4*af*jMax*tf-16*ph3-2*jMax*(jMax*tf_tf+3*vd))/(4*ph4),(29*a0_p3-2*af_p3-33*a0*ph3+6*jj*g1+6*af*jMax*vd+6*a0*p1)/(6*jMax*ph4),(61*a0_p4-76*a0_a0*ph3-16*a0*ph1+30*a0_a0*p1+ph2)/(24*jj*ph4),(a0*(7*a0_p4-10*a0_a0*ph3-4*a0*ph1+6*a0_a0*p1+ph2))/(12*jj*jMax*ph4),(7*a0_p6+af_p6-12*a0_p4*ph3+48*af_p3*jj*g1-8*a0_p3*ph1-72*jj*jMax*(jMax*g1*g1+vd_vd*vd+2*af*g1*vd)-6*af_p4*jMax*vd+36*af_af*jj*vd_vd+9*a0_p4*p1+3*a0_a0*ph2)/(144*jj*jj*ph4)]
                for tt in qroots(poly):
                    disc=(a0_a0+af_af)/(2*jj)+(tt*(2*a0+jMax*tt)-vd)/jMax
                    if disc<0: continue
                    h1=math.sqrt(disc)
                    t=[0]*7; t[0]=tt; t[2]=tt+a0/jMax; t[3]=tf-2*(tt+h1)-(a0+af)/jMax; t[4]=h1; t[6]=h1+af/jMax
                    consider(t,'UDDU',jMax)
        else:
            for tt in qroots([1.0,-tf/2,0.0,pd/(2*jMax)]):
                t=[0]*7; t[0]=tt; t[2]=tt; t[3]=tf-4*tt; t[4]=tt; t[6]=tt
                consider(t,'UDDU',jMax)
        # ===== time_acc0_acc1 =====
        if abs(a0)<EPS and abs(af)<EPS:
            h1=2*aMin*g1+vd_vd+aMax*(2*pd+aMin*tf_tf-2*tf*vf); h2=((aMax-aMin)*(-aMin*vd+aMax*(aMin*tf-vd)))
            if abs(h1)>1e-14:
                jf=h2/h1
                if abs(jf)>1e-14:
                    t=[0]*7; t[0]=aMax/jf; t[1]=(-2*aMax*h1+aMin*aMin*g2)/h2; t[2]=t[0]; t[4]=-aMin/jf; t[5]=tf-(2*t[0]+t[1]+2*t[4]); t[6]=t[4]
                    consider(t,'UDDU',jf)
        else:
            base=2*aMin*g1+vd_vd+aMax*(2*pd+aMin*tf_tf-2*tf*vf)
            if abs(base)>1e-14:
                inner=144*((aMax-aMin)*(-aMin*vd+aMax*(aMin*tf-vd))-af_af*(aMax*tf-vd)+2*af*aMin*(aMax*tf-vd)+a0_a0*(aMin*tf+v0-vf)-2*a0*aMax*(aMin*tf-vd))**2+48*ad*(3*a0_p3-3*af_p3+12*aMax*aMin*(-aMax+aMin)+4*af_af*(aMax+2*aMin)+a0*(-3*af_af+8*af*(aMin-aMax)+6*(aMax*aMax+2*aMax*aMin-aMin*aMin))+6*af*(aMax*aMax-2*aMax*aMin-aMin*aMin)+a0_a0*(3*af-4*(2*aMax+aMin)))*base
                if inner>=0:
                    h1=math.sqrt(inner)
                    jf=-(3*af_af*aMax*tf-3*a0_a0*aMin*tf-6*ad*aMax*aMin*tf+3*aMax*aMin*(aMin-aMax)*tf+3*(a0_a0-af_af)*vd+6*vd*(af*aMin-a0*aMax)+3*(aMax*aMax-aMin*aMin)*vd+h1/4)/(6*base)
                    if abs(jf)>1e-14:
                        t=[0]*7; t[0]=(aMax-a0)/jf; t[1]=(a0_a0-af_af+2*ad*aMin-2*(aMax*aMax-2*aMax*aMin+aMin*aMin+aMin*jf*tf-jf*vd))/(2*(aMax-aMin)*jf); t[2]=aMax/jf; t[4]=-aMin/jf; t[5]=tf-(t[0]+t[1]+t[2]+2*t[4]+af/jf); t[6]=t[4]+af/jf
                        consider(t,'UDDU',jf)
        # ===== time_acc1 (4 solutions) =====
        try:
            h0=math.sqrt(jj*(a0_p4+af_p4-4*af_p3*jMax*tf+6*af_af*jj*tf_tf-4*a0_p3*(af-jMax*tf)+6*a0_a0*(af-jMax*tf)*(af-jMax*tf)+24*af*jj*g1-4*a0*(af_p3-3*af_af*jMax*tf+6*jj*(-pd+tf*vf))-12*jj*(-vd_vd+jMax*tf*g2))/3)/jMax
            h1=math.sqrt((a0_a0+af_af-2*a0*af-2*ad*jMax*tf+2*h0)/jj+tf_tf)
            t=[0]*7; t[0]=-(a0_a0+af_af+2*a0*(jMax*tf-af)-2*jMax*vd+h0)/(2*jMax*(-ad+jMax*tf)); t[2]=(tf-h1)/2-ad/(2*jMax); t[5]=h1; t[6]=tf-(t[0]+t[2]+t[5]); consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        try:
            h0=math.sqrt(jj*(a0_p4+af_p4+4*(af_p3-a0_p3)*jMax*tf+6*af_af*jj*tf_tf+6*a0_a0*(af+jMax*tf)*(af+jMax*tf)+24*af*jj*g1-4*a0*(a0_a0*af+af_p3+3*af_af*jMax*tf+6*jj*(-pd+tf*vf))+12*jj*(vd_vd+jMax*tf*g2))/3)/jMax
            h1=math.sqrt((a0_a0+af_af-2*a0*af+2*ad*jMax*tf+2*h0)/jj+tf_tf)
            t=[0]*7; t[2]=-(a0_a0+af_af-2*a0*af+2*jMax*(vd-a0*tf)+h0)/(2*jMax*(ad+jMax*tf)); t[4]=ad/(2*jMax)+(tf-h1)/2; t[5]=h1; t[6]=tf-(t[5]+t[4]+t[2]); consider(t,'UDUD',jMax)
        except (ValueError,ZeroDivisionError): pass
        # acc1 solution 2 (UDDU) + solution 1 (UDUD)
        try:
            h0a=a0_p3-af_p3-3*a0_a0*aMin+3*aMin*aMin*(a0+jMax*tf)+3*af*aMin*(-aMin-2*jMax*tf)-3*af_af*(-aMin-jMax*tf)-3*jj*(-2*pd-aMin*tf_tf+2*tf*vf)
            h0b=a0_a0+af_af-2*(a0+af)*aMin+2*(aMin*aMin-jMax*(-aMin*tf+vd))
            h0c=a0_p4+3*af_p4-4*(a0_p3+2*af_p3)*aMin+6*a0_a0*aMin*aMin+6*af_af*(aMin*aMin-2*jMax*vd)+12*jMax*(2*aMin*jMax*g1-aMin*aMin*vd+jMax*vd_vd)+24*af*aMin*jMax*vd-4*a0*(af_p3-3*af*aMin*(-aMin-2*jMax*tf)+3*af_af*(-aMin-jMax*tf)+3*jMax*(-aMin*aMin*tf+jMax*(-2*pd-aMin*tf_tf+2*tf*vf)))
            h1=(abs(jMax)/jMax)*math.sqrt(4*h0a*h0a-6*h0b*h0c); h2=6*jMax*h0b
            t=[0]*7; t[2]=(2*h0a+h1)/h2; t[3]=-(a0_a0+af_af-2*(a0+af)*aMin+2*(aMin*aMin+aMin*jMax*tf-jMax*vd))/(2*jMax*(a0-aMin-jMax*t[2])); t[4]=(a0-aMin)/jMax-t[2]; t[5]=tf-(t[2]+t[3]+t[4]+(af-aMin)/jMax); t[6]=(af-aMin)/jMax; consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        try:
            h0a=-a0_p3+af_p3+3*(a0_a0-af_af)*aMax-3*ad*aMax*aMax-6*af*aMax*jMax*tf+3*af_af*jMax*tf+3*jMax*(aMax*aMax*tf+jMax*(-2*pd-aMax*tf_tf+2*tf*vf))
            h0b=a0_a0-af_af+2*ad*aMax+2*jMax*(aMax*tf-vd)
            h0c=a0_p4+3*af_p4-4*(a0_p3+2*af_p3)*aMax+6*a0_a0*aMax*aMax-24*af*aMax*jMax*vd+12*jMax*(2*aMax*jMax*g1+jMax*vd_vd+aMax*aMax*vd)+6*af_af*(aMax*aMax+2*jMax*vd)-4*a0*(af_p3+3*af*aMax*(aMax-2*jMax*tf)-3*af_af*(aMax-jMax*tf)+3*jMax*(aMax*aMax*tf+jMax*(-2*pd-aMax*tf_tf+2*tf*vf)))
            h1=(abs(jMax)/jMax)*math.sqrt(4*h0a*h0a-6*h0b*h0c); h2=6*jMax*h0b
            t=[0]*7; t[2]=-(2*h0a+h1)/h2; t[3]=2*h1/h2; t[4]=(aMax-a0)/jMax+t[2]; t[5]=tf-(t[2]+t[3]+t[4]+(-af+aMax)/jMax); t[6]=(-af+aMax)/jMax; consider(t,'UDUD',jMax)
        except (ValueError,ZeroDivisionError): pass
        # ===== time_acc0 (UDUD + UDDU) =====
        try:
            h1=math.sqrt(ad_ad/(2*jj)-ad*(aMax-a0)/jj+(aMax*tf-vd)/jMax)
            t=[0]*7; t[0]=(aMax-a0)/jMax; t[1]=tf-ad/jMax-2*h1; t[2]=h1; t[4]=(af-aMax)/jMax+h1; consider(t,'UDUD',jMax)
        except (ValueError,ZeroDivisionError): pass
        try:
            h0a=a0_p3+2*af_p3-6*(af_af+aMax*aMax)*aMax-6*(a0+af)*aMax*jMax*tf+9*aMax*aMax*(af+jMax*tf)+3*a0*aMax*(-2*af+3*aMax)+3*a0_a0*(af-2*aMax+jMax*tf)-6*jj*g1+6*(af-aMax)*jMax*vd-3*aMax*jj*tf_tf
            h0b=a0_a0+af_af+2*(aMax*aMax-(a0+af)*aMax+jMax*(vd-aMax*tf))
            h1=(abs(jMax)/jMax)*math.sqrt(4*h0a*h0a-18*h0b*h0b*h0b); h2=6*jMax*h0b
            t=[0]*7; t[0]=(-a0+aMax)/jMax; t[1]=ad/jMax-2*t[0]-(2*h0a-h1)/h2+tf; t[2]=-(2*h0a+h1)/h2; t[3]=(2*h0a-h1)/h2; t[4]=tf-(t[0]+t[1]+t[2]+t[3]); consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # ===== time_none (a0=af=0) + 3-step forms =====
        if abs(v0)<EPS and abs(a0)<EPS and abs(af)<EPS:
            try:
                h1=math.sqrt(tf_tf*vf_vf+(4*pd-tf*vf)**2); jf=4*(4*pd-2*tf*vf+h1)/tf_p3
                t=[0]*7; t[0]=tf/4; t[2]=2*t[0]; t[6]=t[0]; consider(t,'UDDU',jf)
            except (ValueError,ZeroDivisionError): pass
        # 3-step UDDU (T012)
        try:
            h1=math.sqrt(-ad_ad+jMax*(2*(a0+af)*tf-4*vd+jMax*tf_tf))/abs(jMax)
            t=[0]*7; t[0]=(tf-h1+ad/jMax)/2; t[1]=h1; t[2]=(tf-h1-ad/jMax)/2; consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # 3-step UZU (cubic)
        try:
            for tt in qroots([ad_ad, ad_ad*tf, (a0_a0+af_af+10*a0*af)*tf_tf+24*(tf*(af*v0-a0*vf)-pd*ad)+12*vd_vd, -3*tf*((a0_a0+af_af+2*a0*af)*tf_tf-4*vd*(a0+af)*tf+4*vd_vd)]):
                if tt>tf or abs(tf-tt)<1e-14: continue
                jf=ad/(tf-tt)
                if abs(jf)<1e-14: continue
                t=[0]*7; t[0]=(2*(vd-a0*tf)+ad*(tt-tf))/(2*jf*tt); t[1]=tt; t[6]=tf-(t[0]+t[1]); consider(t,'UDDU',jf)
        except (ValueError,ZeroDivisionError): pass
        # 3-step UDU
        try:
            t=[0]*7; t[0]=(ad_ad/jMax+2*(a0+af)*tf-jMax*tf_tf-4*vd)/(4*(ad-jMax*tf)); t[2]=-ad/(2*jMax)+tf/2; t[6]=tf-(t[0]+t[2]); consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # ===== general time_none sub-profiles (a3 != 0) =====
        # UDDU "first acc then constant" (T024) quartic
        try:
            for tt in qroots([1.0,-2*tf,2*vd/jMax+tf_tf,4*(pd-tf*vf)/jMax,(vd_vd+jMax*tf*g2)/jj]):
                t=[0]*7; t[0]=tt; t[2]=(jMax*tt*(tt-tf)+vd)/(jMax*(2*tt-tf)); t[3]=tf-2*tt; t[4]=tt-t[2]; consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # UDUD T0246 (direct)
        try:
            h0=math.sqrt(2*jj*(2*(a0_p3-af_p3-3*af_af*jMax*tf+9*af*jj*tf_tf-3*a0_a0*(af+jMax*tf)+3*a0*(af+jMax*tf)**2+3*jj*(8*pd+jMax*tf_p3-8*tf*vf))**2-3*(a0_a0+af_af-2*af*jMax*tf-2*a0*(af+jMax*tf)-jMax*(jMax*tf_tf+4*v0-4*vf))*(a0_p4+af_p4+4*af_p3*jMax*tf+6*af_af*jj*tf_tf-3*jj*jj*tf_p4*tf_tf/tf_tf-4*a0_p3*(af+jMax*tf)+6*a0_a0*(af+jMax*tf)**2-12*af*jj*(8*pd+jMax*tf_p3-8*tf*v0)+48*jj*vd_vd+48*jj*jMax*tf*g2-4*a0*(af_p3+3*af_af*jMax*tf-9*af*jj*tf_tf-3*jj*(8*pd+jMax*tf_p3-8*tf*vf)))))/jMax
            h1=12*jMax*(-a0_a0-af_af+2*af*jMax*tf+2*a0*(af+jMax*tf)+jMax*(jMax*tf_tf+4*v0-4*vf))
            h2=-4*a0_p3+4*af_p3+12*a0_a0*af-12*a0*af_af+48*jj*pd+12*(a0_a0-af_af)*jMax*tf-24*jj*tf*(v0+vf)+24*ad*jMax*vd
            h3=2*a0_p3-2*af_p3-6*a0_a0*af+6*a0*af_af
            if abs(h1)>1e-12:
                t=[0]*7; t[0]=(h3-48*jj*(tf*vf-pd)-6*(a0_a0+af_af)*jMax*tf+12*a0*af*jMax*tf+6*(a0+3*af+jMax*tf)*tf_tf*jj-h0)/h1; t[2]=(h2+h0)/h1; t[4]=(-h2+h0)/h1; t[6]=(-h3+48*jj*(tf*v0-pd)-6*(a0_a0+af_af)*jMax*tf+12*a0*af*jMax*tf+6*(af+3*a0+jMax*tf)*tf_tf*jj-h0)/h1; consider(t,'UDUD',jMax)
        except (ValueError,ZeroDivisionError): pass
        # UDDU T0234 quartic
        try:
            ph1=af+jMax*tf
            for tt in qroots([1.0,-2*(ad+jMax*tf)/jMax,2*(a0_a0+af_af+jMax*(af*tf+vd)-2*a0*ph1)/jj+tf_tf,2*(a0_p3-af_p3-3*af_af*jMax*tf+3*a0*ph1*(ph1-a0)-6*jj*(-pd+tf*vf))/(3*jj*jMax),(a0_p4+af_p4+4*af_p3*jMax*tf-4*a0_p3*ph1+6*a0_a0*ph1*ph1+24*jj*af*g1-4*a0*(af_p3+3*af_af*jMax*tf+6*jj*(-pd+tf*vf))+6*jj*af_af*tf_tf+12*jj*(vd_vd+jMax*tf*g2))/(12*jj*jj)]):
                den=(-ad+jMax*(2*tt-tf))
                if abs(den)<1e-14: continue
                t=[0]*7; t[0]=tt; t[2]=(ad_ad+2*jMax*(-a0*tf-ad*tt+jMax*tt*(tt-tf)+vd))/(2*jMax*den); t[3]=ad/jMax+tf-2*tt; t[4]=tf-(tt+t[2]+t[3]); consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # UDDU T3456 (direct)
        try:
            h1n=3*jMax*(ad_ad+2*jMax*(a0*tf-vd)); h2n=ad_ad+2*jMax*(a0*tf-vd)
            h0=math.sqrt(4*(2*(a0_p3-af_p3)-6*a0_a0*(af-jMax*tf)+6*jj*g1+3*a0*(2*af_af-2*jMax*af*tf+jj*tf_tf)+6*ad*jMax*vd)**2-18*h2n*h2n*h2n)/h1n*(abs(jMax)/jMax)
            t=[0]*7; t[3]=(af_p3-a0_p3+3*(af_af-a0_a0)*jMax*tf-3*ad*(a0*af+2*jMax*vd)-6*jj*g2)/h1n; t[4]=(tf-t[3]-h0)/2-ad/(2*jMax); t[5]=h0; t[6]=(tf-t[3]+ad/jMax-h0)/2; consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # UDDU T2346 quartic
        try:
            ph1=ad_ad+2*(af+a0)*jMax*tf-jMax*(jMax*tf_tf+4*vd); ph2=jMax*tf_tf*g1-vd*(-2*pd-tf*v0+3*tf*vf); ph3=5*af_af-8*af*jMax*tf+2*jMax*(2*jMax*tf_tf-vd); ph4=jj*tf_p4-2*vd_vd+8*jMax*tf*(-pd+tf*vf); ph5=(5*af_p4-8*af_p3*jMax*tf-12*af_af*jMax*(jMax*tf_tf+vd)+24*af*jj*(-2*pd+jMax*tf_p3+2*tf*vf)-6*jj*ph4); ph6=-vd_vd+jMax*tf*(-2*pd+3*tf*v0-tf*vf)-af*g2
            if abs(ph1)>1e-12:
                for tt in qroots([1.0,-(4*(a0_p3-af_p3)-12*a0_a0*(af-jMax*tf)+6*a0*(2*af_af-2*af*jMax*tf+jMax*(jMax*tf_tf-2*vd))+6*af*jMax*(3*jMax*tf_tf+2*vd)-6*jj*(-4*pd+jMax*tf_p3-2*tf*v0+6*tf*vf))/(3*jMax*ph1),-(-a0_p4-af_p4+4*a0_p3*(af-jMax*tf)+a0_a0*(-6*af_af+8*af*jMax*tf-4*jMax*(jMax*tf_tf-vd))+2*af_af*jMax*(jMax*tf_tf+2*vd)-4*af*jj*(-3*pd+jMax*tf_p3+2*tf*v0+tf*vf)+jj*(jj*tf_p4-8*vd_vd+4*jMax*tf*(-3*pd+tf*v0+2*tf*vf))+2*a0*(2*af_p3-2*af_af*jMax*tf+af*jMax*(-3*jMax*tf_tf-4*vd)+jj*(-6*pd+jMax*tf_p3-4*tf*v0+10*tf*vf)))/(jj*ph1),-(a0_p5-af_p5+af_p4*jMax*tf-5*a0_p4*(af-jMax*tf)+2*a0_p3*ph3+4*af_p3*jMax*(jMax*tf_tf+vd)+12*jj*af*ph6-2*a0_a0*(5*af_p3-9*af_af*jMax*tf-6*af*jMax*vd+6*jj*(-2*pd-tf*v0+3*tf*vf))-12*jj*jMax*ph2+a0*ph5)/(3*jj*jMax*ph1),-(-a0_p6-af_p6+6*a0_p5*(af-jMax*tf)-48*af_p3*jj*g1+72*jj*jMax*(jMax*g1*g1+vd_vd*vd+2*af*g1*vd)-3*a0_p4*ph3-36*af_af*jj*vd_vd+6*af_p4*jMax*vd+4*a0_p3*(5*af_p3-9*af_af*jMax*tf-6*af*jMax*vd+6*jj*(-2*pd-tf*v0+3*tf*vf))-3*a0_a0*ph5+6*a0*(af_p5-af_p4*jMax*tf-4*af_p3*jMax*(jj*tf_tf/jMax+vd)+12*jj*(-af*ph6+jMax*ph2)))/(18*jj*jj*ph1)]):
                    disc2=2*ad_ad+4*jMax*(ad*tt+a0*tf+jMax*tt*(tt-tf)-vd)
                    if disc2<0: continue
                    h1=math.sqrt(disc2)/abs(jMax)
                    t=[0]*7; t[2]=tt; t[3]=tf-2*tt-ad/jMax-h1; t[4]=h1/2; t[6]=tf-(tt+t[3]+t[4]); consider(t,'UDDU',jMax)
        except (ValueError,ZeroDivisionError): pass
        # UDUD T0124 quartic
        try:
            ph0=-2*pd-tf*v0+3*tf*vf; ph1=-ad+jMax*tf; ph2=jMax*tf_tf*g1-vd*ph0; ph3=5*af_af+2*jMax*(2*jMax*tf_tf-vd-4*af*tf); ph4=jj*tf_p4-2*vd_vd+8*jMax*tf*(-pd+tf*vf); ph5=(5*af_p4-8*af_p3*jMax*tf-12*af_af*jMax*(jMax*tf_tf+vd)+24*af*jj*(-2*pd+jMax*tf_p3+2*tf*vf)-6*jj*ph4); ph6=-vd_vd+jMax*tf*(-2*pd+3*tf*v0-tf*vf); ph7=3*jj*ph1*ph1
            if abs(ph1)>1e-12 and abs(ph7)>1e-12:
                for tt in qroots([1.0,(4*af*tf-2*jMax*tf_tf-4*vd)/ph1,(-2*(a0_p4+af_p4)+8*af_p3*jMax*tf+6*af_af*jj*tf_tf+8*a0_p3*(af-jMax*tf)-12*a0_a0*(af-jMax*tf)*(af-jMax*tf)-12*af*jj*(-pd+jMax*tf_p3-2*tf*v0+3*tf*vf)+2*a0*(4*af_p3-12*af_af*jMax*tf+9*af*jj*tf_tf-3*jj*(2*pd+jMax*tf_p3-2*tf*vf))+3*jj*(jj*tf_p4+4*vd_vd-4*jMax*tf*(pd+tf*v0-2*tf*vf)))/ph7,(-a0_p5+af_p5-af_p4*jMax*tf+5*a0_p4*(af-jMax*tf)-2*a0_p3*ph3-4*af_p3*jMax*(jMax*tf_tf+vd)+12*af_af*jj*g2-12*af*jj*ph6+2*a0_a0*(5*af_p3-9*af_af*jMax*tf-6*af*jMax*vd+6*jj*ph0)+12*jj*jMax*ph2+a0*(-5*af_p4+8*af_p3*jMax*tf+12*af_af*jMax*(jMax*tf_tf+vd)-24*af*jj*(-2*pd+jMax*tf_p3+2*tf*vf)+6*jj*ph4))/(jMax*ph7),-(a0_p6+af_p6-6*a0_p5*(af-jMax*tf)+48*af_p3*jj*g1-72*jj*jMax*(jMax*g1*g1+vd_vd*vd+2*af*g1*vd)+3*a0_p4*ph3-6*af_p4*jMax*vd+36*af_af*jj*vd_vd-4*a0_p3*(5*af_p3-9*af_af*jMax*tf-6*af*jMax*vd+6*jj*ph0)+3*a0_a0*ph5-6*a0*(af_p5-af_p4*jMax*tf-4*af_p3*jMax*(jMax*tf_tf+vd)+12*jj*(af_af*g2-af*ph6+jMax*ph2)))/(6*jj*ph7)]):
                    disc=ad_ad/(2*jj)+(a0*(tt+tf)-af*tt+jMax*tt*tf-vd)/jMax
                    if disc<0: continue
                    h1=math.sqrt(disc)
                    t=[0]*7; t[0]=tt; t[1]=tf-ad/jMax-2*h1; t[2]=h1; t[4]=ad/jMax+h1-tt; consider(t,'UDUD',jMax)
        except (ValueError,ZeroDivisionError): pass
    return best

def ruckig_sync(dofs, tf_target=None):
    n=len(dofs)
    inp=InputParameter(n)
    inp.current_position=[0.0]*n; inp.current_velocity=[d[0] for d in dofs]; inp.current_acceleration=[d[1] for d in dofs]
    inp.target_position=[d[4] for d in dofs]; inp.target_velocity=[d[2] for d in dofs]; inp.target_acceleration=[d[3] for d in dofs]
    inp.max_velocity=[d[5] for d in dofs]; inp.max_acceleration=[d[6] for d in dofs]; inp.max_jerk=[d[7] for d in dofs]
    tr=Trajectory(n); Ruckig(n).calculate(inp,tr); return t

# Verify: 2-DoF sync. DoF0 = a hard move (sets tsync); DoF1 = an easy move that must stretch to tsync via step2.
rng=np.random.RandomState(3); npass=nfail=nskip=0; misses=[]
for it in range(2500):
    made=[]
    for k in range(2):
        vmax=rng.uniform(0.4,3.5); amax=rng.uniform(0.4,2.5); jmax=rng.uniform(0.8,4.5)
        v0=rng.uniform(-0.8,0.8)*vmax; a0=rng.uniform(-0.7,0.7)*amax
        vf=rng.uniform(-0.8,0.8)*vmax; af=rng.uniform(-0.6,0.6)*amax
        pf=(rng.uniform(20,40) if k==0 else rng.uniform(-6,6))  # DoF0 far -> dominates tsync
        made.append((v0,a0,vf,af,pf,vmax,amax,jmax))
    try:
        tr=ruckig_sync(made)
    except Exception:
        nskip+=1; continue
    tsync=tr.duration
    d=made[1]
    r=step2(0,d[0],d[1],d[4],d[2],d[3],d[5],d[6],d[7],tsync)
    if r is not None: npass+=1
    else:
        nfail+=1
        if len(misses)<8: misses.append((tuple(round(x,3) for x in d),round(tsync,4)))
print(f"step2 (DoF1 reach-in-tsync): {npass}/{npass+nfail} solved ({nskip} skipped)")
for m in misses: print("  miss:", m)
