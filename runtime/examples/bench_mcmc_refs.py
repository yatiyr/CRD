# v12-q MCMC peer bench: PyMC NUTS sampling throughput (draws/sec, ESS/sec) on a 10-D standard Gaussian.
import time
import numpy as np
import pymc as pm
import arviz as az

with pm.Model():
    pm.Normal("x", 0.0, 1.0, shape=10)
    pm.sample(draws=50, tune=50, chains=1, cores=1, progressbar=False, compute_convergence_checks=False)  # compile
    t0 = time.time()
    idata = pm.sample(draws=4000, tune=1000, chains=1, cores=1, progressbar=False,
                      compute_convergence_checks=False)
    t = time.time() - t0

ess = float(az.ess(idata).x.values.ravel()[0])
print("PyMC NUTS  %9.0f draws/s   %9.0f ess/s   (%.0f ESS)" % (5000 / t, ess / t, ess))
