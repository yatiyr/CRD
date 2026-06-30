import numpy as np
from scipy.integrate import quad
import warnings; warnings.filterwarnings("ignore")
fns=[("exp(x)cos(2x)",lambda x:np.exp(x)*np.cos(2*x),0.0,1.0),
     ("sin(x)",lambda x:np.sin(x),0.0,np.pi),
     ("exp(-x^2)",lambda x:np.exp(-x*x),0.0,3.0),
     ("1/(1+x^2)",lambda x:1.0/(1+x*x),0.0,5.0),
     ("x*exp(-x)",lambda x:x*np.exp(-x),0.0,10.0)]
print("// scipy.integrate.quad reference values (gen_adaptive_refs.py)")
vals=[]
for name,f,a,b in fns:
    v,e=quad(f,a,b,epsabs=1e-13,epsrel=1e-13)
    vals.append(v)
print("constexpr double ref_quad[] = {%s};"%(", ".join("%.17g"%v for v in vals)))
# GK21 single-panel ref on exp(x)cos(2x) over [0,1]
print("constexpr double ref_gk21_val = 0.562449792050565;")
# Lyness-Kaganove: narrow peak exp(-1e6 (x-0.3)^2) on [0,1] — true integral
true_peak=quad(lambda x:np.exp(-1e6*(x-0.3)**2),0,1,epsabs=1e-15,epsrel=1e-15,limit=500)[0]
print("constexpr double ref_peak_true = %.17g; // narrow-peak true integral (~sqrt(pi/1e6))"%true_peak)
