#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-lbfgs-ref.sh -- stand up the liblbfgs reference oracle for the crd-hesap
# v7-d L-BFGS head-to-head (Phase 3.1.6 v7-d).
#
# WHY liblbfgs: it is THE reference C implementation of L-BFGS (Naoaki Okazaki's
# port of Nocedal's L-BFGS), the de-facto unconstrained quasi-Newton peer, and --
# critically -- its DEFAULT line search IS More-Thuente (MINPACK dcsrch/dcstep).
# That makes it the ideal EVAL-COUNT peer: if Cerid-L-BFGS takes more fn/grad
# evaluations than liblbfgs on the same problem at matched m + matched stopping,
# the gap is in OUR More-Thuente interpolation, not the L-BFGS recursion. It is a
# pure-C library (clean wall-clock, no Python callback overhead -- unlike scipy).
#
# LICENSE: liblbfgs is MIT (permissive) -- no GPL encumbrance. Still treated as a
# LOCAL-ONLY, NON-DISTRIBUTED benchmarking oracle: gated behind the dev-only
# CRD_BUILD_HESAP_VS_LBFGS flag (default OFF), NEVER linked into a shipped Cerid
# artifact, NEVER built in CI release. Source is cloned into a gitignored external/
# dir (not vendored into git).
#
# Run from WSL/Linux:  bash scripts/setup-lbfgs-ref.sh
# Idempotent: skips the clone/build if the static lib is already present.
# ---------------------------------------------------------------------------
set -euo pipefail

ext_dir="${CRD_LBFGS_DIR:-$HOME/cerid-deps/liblbfgs}"
src_dir="$ext_dir/src"
inst_dir="$ext_dir/install"

if [ -f "$inst_dir/lib/liblbfgs.a" ] && [ -f "$inst_dir/include/lbfgs.h" ]; then
    echo "[lbfgs-ref] already built: $inst_dir/lib/liblbfgs.a"
else
    mkdir -p "$ext_dir"
    if [ ! -d "$src_dir/.git" ]; then
        echo "[lbfgs-ref] cloning chokkan/liblbfgs ..."
        rm -rf "$src_dir"
        git clone --depth 1 https://github.com/chokkan/liblbfgs.git "$src_dir"
    fi
    echo "[lbfgs-ref] building static lib (double precision, SSE2 — its release default) ..."
    # liblbfgs ships an autotools build, but the library is a SINGLE translation unit
    # (lib/lbfgs.c). Build it directly to avoid autoreconf churn — double precision,
    # SSE2-vectorized (its release default; a FAIR wall-clock peer). lbfgs.c #include
    # <config.h> unconditionally; the stub must define HAVE_EMMINTRIN_H so the SSE2
    # double arithmetic header pulls <emmintrin.h> (else _mm_set_pd is undefined).
    mkdir -p "$inst_dir/lib" "$inst_dir/include"
    printf '/* minimal config.h stub for the direct (non-autotools) build */\n#define HAVE_EMMINTRIN_H 1\n#define HAVE_XMMINTRIN_H 1\n#define HAVE_MEMALIGN 1\n' \
        > "$ext_dir/config.h"
    gcc -O2 -DNDEBUG -DUSE_SSE=1 -msse2 -include emmintrin.h \
        -I"$ext_dir" -I"$src_dir/include" -I"$src_dir/lib" \
        -c "$src_dir/lib/lbfgs.c" -o "$ext_dir/lbfgs.o"
    ar rcs "$inst_dir/lib/liblbfgs.a" "$ext_dir/lbfgs.o"
    cp "$src_dir/include/lbfgs.h" "$inst_dir/include/lbfgs.h"
fi

echo "[lbfgs-ref] lib : $inst_dir/lib/liblbfgs.a"
echo "[lbfgs-ref] hdr : $inst_dir/include/lbfgs.h"
echo ""
echo "[lbfgs-ref] OK. Configure with:"
echo "  cmake --preset linux-gcc-release -DCRD_BUILD_HESAP_VS_REFERENCE=ON \\"
echo "        -DCRD_BUILD_HESAP_VS_LBFGS=ON -DCRD_LBFGS_DIR=$inst_dir"
