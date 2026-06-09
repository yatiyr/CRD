#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-lbfgsb-ref.sh -- build the L-BFGS-B reference C (Stephen Becker's
# translation of the Zhu-Byrd-Lu-Nocedal Fortran, the exact code scipy wraps)
# into a static lib, for the v7-d-3 DIFFERENTIAL-TEST HARNESS.
#
# WHY: the faithful L-BFGS-B port is a confirmed bug farm (the 1-based↔f2c
# 0-based-BLAS pointer convention produced 4 off-by-ones in the EASY routines;
# manual audit is NOT verification). The only honest way to port it is to diff
# every Cerid routine against this reference on identical inputs (reals AND the
# integer index arrays), in bug-hiding regimes (iupdat>m ring-wrap, breakpoint
# ties). This lib is that oracle. LOCAL-ONLY, never CI/shipped; the reference is
# used ONLY to verify the from-principles Cerid port (PRINCIPLES.md tak-cikar:
# Cerid SHIPS its own L-BFGS-B; the C is a test oracle, not a dependency).
#
# Run (WSL):  bash scripts/setup-lbfgsb-ref.sh
# ---------------------------------------------------------------------------
set -euo pipefail

ext_dir="${CRD_LBFGSB_REF_DIR:-$HOME/cerid-deps/lbfgsb-ref}"
src_dir="$ext_dir/src"
inst_dir="$ext_dir/install"

if [ -f "$inst_dir/lib/liblbfgsb_ref.a" ] && [ -f "$inst_dir/include/lbfgsb.h" ]; then
    echo "[lbfgsb-ref] already built: $inst_dir/lib/liblbfgsb_ref.a"
else
    mkdir -p "$ext_dir"
    if [ ! -d "$src_dir/.git" ]; then
        echo "[lbfgsb-ref] cloning stephenbeckr/L-BFGS-B-C ..."
        rm -rf "$src_dir"
        git clone --depth 1 https://github.com/stephenbeckr/L-BFGS-B-C.git "$src_dir"
    fi
    echo "[lbfgsb-ref] building reference static lib (the algorithm TUs, no drivers) ..."
    mkdir -p "$inst_dir/lib" "$inst_dir/include"
    objs=""
    # All algorithm/support TUs EXCEPT driver1/2/3.c (each has a main()).
    for tu in lbfgsb subalgorithms linesearch linpack miniCBLAS print timer; do
        gcc -O2 -DNDEBUG -I"$src_dir/src" -c "$src_dir/src/$tu.c" -o "$ext_dir/$tu.o"
        objs="$objs $ext_dir/$tu.o"
    done
    ar rcs "$inst_dir/lib/liblbfgsb_ref.a" $objs
    cp "$src_dir/src/lbfgsb.h" "$inst_dir/include/lbfgsb.h"
fi

echo "[lbfgsb-ref] lib : $inst_dir/lib/liblbfgsb_ref.a"
echo "[lbfgsb-ref] hdr : $inst_dir/include/lbfgsb.h"
echo "[lbfgsb-ref] symbols (the diff-test oracle routines):"
nm "$inst_dir/lib/liblbfgsb_ref.a" 2>/dev/null | grep -E " T (cauchy|subsm|formk|formt|cmprlb|bmv|matupd|active|freev|projgr|hpsolb|dpofa|dtrsl|mainlb)$" | head -20 || true
