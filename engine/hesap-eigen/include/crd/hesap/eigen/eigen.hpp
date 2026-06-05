#pragma once

// crd-hesap-eigen umbrella — Phase 3.1.6 v6. SPARSE EIGENVALUE, matrix-free over crd::hesap::LinearOp<T>.
// The moat differentiator: eigenpairs bit-identical across {1,2,4,8} workers (no ARPACK/PRIMME/SLEPc carries
// it). Rayleigh-Ritz reuses the dense eigensolvers (crd-hesap-dense); shift-invert reuses the v5 direct
// factors (crd-hesap-direct); LOBPCG/Jacobi-Davidson use crd-hesap-preconditioners + crd-hesap-iterative.
//
// v6-a substrate (this header set): the eigenproblem spec. Methods land per slice:
//   v6-a symmetric Lanczos · v6-b thick-restart Lanczos · v6-c Arnoldi + Krylov-Schur ·
//   v6-d shift-invert · v6-e LOBPCG · v6-f Jacobi-Davidson · v6-g FEAST · v6-h IRLBA (sparse SVD).

#include <crd/hesap/eigen/arnoldi.hpp>       // v6-c: nonsymmetric Arnoldi
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/eigen/lanczos.hpp>        // v6-a: symmetric Lanczos
#include <crd/hesap/eigen/shift_invert.hpp>  // v6-d: shift-invert (interior eigenvalues via a v5 factor)
#include <crd/hesap/eigen/thick_restart.hpp> // v6-b: thick-restart Lanczos
