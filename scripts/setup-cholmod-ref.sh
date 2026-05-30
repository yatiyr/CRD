#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-cholmod-ref.sh -- stand up the SuiteSparse CHOLMOD reference oracle for
# the crd-hesap v5a supernodal-Cholesky head-to-head (Phase 3.1.6 v5a-2).
#
# WHY CHOLMOD: it is THE gold-standard supernodal sparse Cholesky (Davis et al.,
# the engine behind MATLAB's chol, Eigen's CholmodSupernodalLLT, scikit-sparse).
# Our v5a benches so far have only raced Eigen's SIMPLICIAL LLT (a weaker,
# scalar peer). CHOLMOD is the real supernodal floor — beating it is the claim.
#
# LICENSE NOTICE: SuiteSparse CHOLMOD's Core/Cholesky modules are LGPL-2.1+, but
# the SUPERNODAL + Modify + MatrixOps modules are GPL-2+. A bench that forces the
# supernodal path and links libcholmod is therefore GPL-encumbered. It is treated
# as a LOCAL-ONLY, NON-DISTRIBUTED benchmarking oracle: gated behind the dev-only
# CRD_BUILD_HESAP_VS_CHOLMOD flag (default OFF), NEVER linked into any shipped
# Cerid artifact, NEVER built in CI release. The system .so is apt-installed (not
# vendored into git). Any publication using it must cite:
#   Y. Chen, T. A. Davis, W. W. Hager, S. Rajamanickam. Algorithm 887: CHOLMOD,
#   supernodal sparse Cholesky factorization and update/downdate. ACM TOMS 35(3),
#   2008.
#
# Run from WSL/Linux:  bash scripts/setup-cholmod-ref.sh
# Requires apt (sudo). Idempotent: skips install if the headers/libs are present.
#
# FAIR-FIGHT NOTE: CHOLMOD's dense supernode panels run through BLAS-3 (dgemm/
# dsyrk/dtrsm). Ubuntu defaults libblas.so.3 to the SINGLE-THREADED REFERENCE
# netlib BLAS, which would make CHOLMOD look artificially slow. This script
# installs libopenblas-dev and switches the BLAS/LAPACK alternative to OpenBLAS
# so the comparison is honest (both sides on an optimized, multi-threaded BLAS).
# ---------------------------------------------------------------------------
set -euo pipefail

need_install=0
if [ ! -f /usr/include/suitesparse/cholmod.h ]; then need_install=1; fi
if [ ! -f /usr/include/suitesparse/amd.h ]; then need_install=1; fi

if [ "$need_install" -eq 1 ]; then
    echo "[cholmod-ref] installing libsuitesparse-dev + libopenblas-dev (sudo apt) ..."
    sudo apt-get update -qq
    sudo apt-get install -y libsuitesparse-dev libopenblas-dev
else
    echo "[cholmod-ref] SuiteSparse headers already present; ensuring OpenBLAS ..."
    if ! dpkg -l | grep -qiE '\blibopenblas-dev\b'; then
        sudo apt-get install -y libopenblas-dev
    fi
fi

# Switch BLAS/LAPACK to OpenBLAS so CHOLMOD's dense panels are not on reference netlib.
blas_path="$(readlink -f /usr/lib/x86_64-linux-gnu/libblas.so.3 || true)"
case "$blas_path" in
    *openblas*) echo "[cholmod-ref] BLAS already OpenBLAS: $blas_path" ;;
    *)
        echo "[cholmod-ref] switching libblas/liblapack alternative to OpenBLAS ..."
        ob_blas="$(update-alternatives --list libblas.so.3-x86_64-linux-gnu 2>/dev/null | grep -i openblas | head -1 || true)"
        ob_lapack="$(update-alternatives --list liblapack.so.3-x86_64-linux-gnu 2>/dev/null | grep -i openblas | head -1 || true)"
        [ -n "$ob_blas" ] && sudo update-alternatives --set libblas.so.3-x86_64-linux-gnu "$ob_blas"
        [ -n "$ob_lapack" ] && sudo update-alternatives --set liblapack.so.3-x86_64-linux-gnu "$ob_lapack"
        ;;
esac

echo "[cholmod-ref] cholmod : $(ls /usr/lib/x86_64-linux-gnu/libcholmod.so* | head -1)"
echo "[cholmod-ref] BLAS    : $(readlink -f /usr/lib/x86_64-linux-gnu/libblas.so.3)"
echo "[cholmod-ref] LAPACK  : $(readlink -f /usr/lib/x86_64-linux-gnu/liblapack.so.3)"
echo ""
echo "[cholmod-ref] OK. Configure with:"
echo "  cmake --preset linux-relwithdebinfo -DCRD_BUILD_HESAP_VS_REFERENCE=ON -DCRD_BUILD_HESAP_VS_CHOLMOD=ON"
echo "  link line: -lcholmod -lamd -lcolamd -lsuitesparseconfig (+ system BLAS/LAPACK via CHOLMOD)"
