#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-ilupack-ref.sh -- stand up the ILUPACK V2.4 reference oracle for the
# crd-hesap v4j multilevel-ILU head-to-head (Phase 3.1.6 v4j-2 / v4j-3).
#
# LICENSE NOTICE: ILUPACK is "freely available for scientific (non-commercial)
# use ... only for the purpose of internal research excluding any commercial
# use" (http://ilupack.tu-bs.de/copyright.shtml). It is BINARY-ONLY (precompiled
# .a libraries, no source). Therefore it is treated as a LOCAL-ONLY, NON-
# DISTRIBUTED benchmarking oracle: it lives under the gitignored external/
# directory, is gated behind the dev-only CRD_BUILD_HESAP_VS_ILUPACK flag, is
# NEVER vendored into git, NEVER fetched in CI, and NEVER linked into any
# shipped Cerid artifact. Any scientific publication using it must cite:
#   M. Bollhoefer and Y. Saad. Multilevel preconditioners constructed from
#   inverse-based ILUs. SIAM J. Sci. Comput. 27(5):1627-1650, 2006.
#
# Run from WSL/Linux:  bash scripts/setup-ilupack-ref.sh
# Requires: curl, python3 (zip extract), gcc, gfortran (apt install gfortran).
# Idempotent: skips download/extract if already present.
# ---------------------------------------------------------------------------
set -euo pipefail

# Repo root = parent of this script's dir (works whether invoked from WSL path
# or the /mnt/d mount).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EXT="$REPO_ROOT/external/ilupack"
PKG="ilupackV2.4_GNU64_MUMPS"   # GNU compiler, 64-bit addr / 32-bit int, MUMPS matching (no HSL)
URL="http://www.icm.tu-bs.de/~bolle/ilupack/download/${PKG}.zip"
DIST="$EXT/dist/$PKG"
LIB="$DIST/lib/GNU64"

mkdir -p "$EXT"
cd "$EXT"

if [ ! -f "${PKG}.zip" ]; then
    echo "[ilupack-ref] downloading $URL ..."
    curl -fSL -m 300 -o "${PKG}.zip" "$URL"
else
    echo "[ilupack-ref] zip already present, skipping download."
fi

if [ ! -d "$DIST" ]; then
    echo "[ilupack-ref] extracting ..."
    python3 -c "import zipfile; zipfile.ZipFile('${PKG}.zip').extractall('dist')"
else
    echo "[ilupack-ref] already extracted, skipping."
fi

# Link smoke-test: build the bundled double-general example against the lib.
# Proves the oracle links + solves before any bench wires into it.
if ! command -v gfortran >/dev/null 2>&1; then
    echo "[ilupack-ref] ERROR: gfortran not found. Install with: sudo apt-get install -y gfortran" >&2
    exit 1
fi

echo "[ilupack-ref] link smoke-test (dmaingnl on lnsp3937.rua) ..."
cd "$DIST/simple_examples"
gcc -c -O2 -fPIC -m64 -D__UNDERSCORE__ -mcmodel=medium -I ../include dmaingnl.c -o dmaingnl.o
gfortran -O2 -fPIC -m64 -mcmodel=medium -o dmaingnl.out dmaingnl.o \
    -L ../lib/GNU64/ -lilupack -lmumps -lamd -lmetis -lsparspak -llapack -lblaslike -lblas
./dmaingnl.out 1e-2 5 10 ../lnsp3937.rua | grep -E "factorization successful|iteration successful" \
    || { echo "[ilupack-ref] ERROR: smoke solve did not converge" >&2; exit 1; }

echo ""
echo "[ilupack-ref] OK. Oracle ready."
echo "  include dir : $DIST/include"
echo "  lib dir     : $LIB"
echo "  link line   : -L$LIB -lilupack -lmumps -lamd -lmetis -lsparspak -llapack -lblaslike -lblas (link with gfortran)"
echo "  C API       : D{GNL,SPD,SYM}AMG{init,factor,solver,sol,delete}; param.condest = inverse-based pivot bound kappa"
