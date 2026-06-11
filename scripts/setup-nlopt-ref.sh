#!/usr/bin/env bash
# v7-p-2..4 — build the NLopt reference ORACLE for the Powell-code differential harnesses (COBYLA first).
# The L-BFGS-B playbook: the port is adjudicated per-routine + end-to-end against the compiled reference.
#
# Strategy:
#   1. cmake-build stock static libnlopt.a (gives the public NLOPT_LN_COBYLA end-to-end oracle + the util objects).
#   2. Compile an EXPOSED-STATICS TU (`#define static` + #include cobyla.c) so trstlp/cobylb/cobyla are
#      linkable for per-routine diffing — WITHOUT patching the reference source.
#   3. Strip the stock cobyla object out of a copy of the lib (`ar d`) so the difftest links
#      cobyla_exposed.o + libnlopt_nocobyla.a with no duplicate symbols.
#
# Run on WSL: bash scripts/setup-nlopt-ref.sh
set -e

NLOPT="$HOME/cerid-deps/nlopt"
BUILD="$HOME/cerid-deps/nlopt-ref-build"

if [ ! -d "$NLOPT/.git" ]; then
    git clone --depth 1 https://github.com/stevengj/nlopt.git "$NLOPT"
fi

cmake -S "$NLOPT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DNLOPT_PYTHON=OFF -DNLOPT_OCTAVE=OFF -DNLOPT_MATLAB=OFF -DNLOPT_GUILE=OFF -DNLOPT_SWIG=OFF \
      -DNLOPT_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"${CMAKE_BUILD_PARALLEL_LEVEL:-8}" >/dev/null

LIB=$(find "$BUILD" -name 'libnlopt.a' | head -1)
[ -n "$LIB" ] || { echo "FAIL: libnlopt.a not built"; exit 1; }

# Exposed-statics TU (separate compilation; never linked together with the stock cobyla object) + the
# end-to-end shim: an apples-to-apples cobyla()-layer entry building the minimal func_wrap_state/stop the
# driver consults (con_tol zeros; no rescaling — the layer minimize_cobyla mirrors).
cat > "$BUILD/cobyla_exposed.c" <<'EOF'
/* Externalize the static functions of the reference for per-routine differential testing. */
#define static
#include "cobyla.c"

typedef int (*crd_calcfc_plain)(int n, int m, const double *x, double *f, double *con);
crd_calcfc_plain crd_plain_cb;
int crd_calcfc_adapter(int n, int m, double *x, double *f, double *con, func_wrap_state *state)
{
    (void)state;
    return crd_plain_cb(n, m, x, f, con);
}
nlopt_result crd_cobyla_e2e(int n, int m, double *x, double *minf, double rhobeg, double rhoend,
                            int maxeval, double ftol_rel, double ftol_abs,
                            const double *lb, const double *ub, crd_calcfc_plain cb, int *nevals_out)
{
    func_wrap_state state;
    nlopt_stopping stop;
    int nevals = 0;
    nlopt_result rc;
    double *ctol = (double *)calloc((size_t)(m + 1), sizeof(double));
    memset(&state, 0, sizeof(state));
    memset(&stop, 0, sizeof(stop));
    stop.n = (unsigned)n;
    stop.nevals_p = &nevals;
    stop.maxeval = maxeval;
    stop.minf_max = -HUGE_VAL;
    stop.ftol_rel = ftol_rel;
    stop.ftol_abs = ftol_abs;
    state.con_tol = ctol;
    crd_plain_cb = cb;
    rc = cobyla(n, m, x, minf, rhobeg, rhoend, &stop, lb, ub, 0, crd_calcfc_adapter, &state);
    *nevals_out = nevals;
    free(ctol);
    return rc;
}
EOF
gcc -O2 -c "$BUILD/cobyla_exposed.c" -o "$BUILD/cobyla_exposed.o" \
    -I"$NLOPT/src/algs/cobyla" -I"$NLOPT/src/util" -I"$NLOPT/src/api" \
    -I"$BUILD" -I"$BUILD/src/api"

# A lib copy with the stock cobyla member removed (avoids duplicate symbols next to the exposed TU).
cp "$LIB" "$BUILD/libnlopt_nocobyla.a"
COBJ=$(ar t "$BUILD/libnlopt_nocobyla.a" | grep -i '^cobyla' | head -1)
[ -n "$COBJ" ] && ar d "$BUILD/libnlopt_nocobyla.a" "$COBJ"

# ---- NEWUOA oracle (v7-p-3): same recipe. The e2e shim passes NULL bounds (the classic unconstrained
# algorithm — the pinned port scope) and xtol_rel (the reference computes rhoend = xtol_rel*rhobeg itself).
cat > "$BUILD/newuoa_exposed.c" <<'EOF'
/* Externalize the static functions of the reference for per-routine differential testing. */
#define static
#include "newuoa.c"

typedef double (*crd_newuoa_cb)(int n, const double *x);
crd_newuoa_cb crd_newuoa_plain_cb;
double crd_newuoa_adapter(int n, const double *x, void *data)
{
    (void)data;
    return crd_newuoa_plain_cb(n, x);
}
nlopt_result crd_newuoa_e2e(int n, int npt, double *x, double *minf, double rhobeg, double xtol_rel,
                            int maxeval, double ftol_rel, double ftol_abs, crd_newuoa_cb cb, int *nevals_out)
{
    nlopt_stopping stop;
    int nevals = 0;
    nlopt_result rc;
    memset(&stop, 0, sizeof(stop));
    stop.n = (unsigned)n;
    stop.nevals_p = &nevals;
    stop.maxeval = maxeval;
    stop.minf_max = -HUGE_VAL;
    stop.ftol_rel = ftol_rel;
    stop.ftol_abs = ftol_abs;
    stop.xtol_rel = xtol_rel;
    crd_newuoa_plain_cb = cb;
    rc = newuoa(n, npt, x, NULL, NULL, rhobeg, &stop, minf, crd_newuoa_adapter, NULL);
    *nevals_out = nevals;
    return rc;
}
EOF
gcc -O2 -c "$BUILD/newuoa_exposed.c" -o "$BUILD/newuoa_exposed.o" \
    -I"$NLOPT/src/algs/newuoa" -I"$NLOPT/src/util" -I"$NLOPT/src/api" \
    -I"$BUILD" -I"$BUILD/src/api"
cp "$LIB" "$BUILD/libnlopt_nonewuoa.a"
NOBJ=$(ar t "$BUILD/libnlopt_nonewuoa.a" | grep -i '^newuoa' | head -1)
[ -n "$NOBJ" ] && ar d "$BUILD/libnlopt_nonewuoa.a" "$NOBJ"

# ---- BOBYQA oracle (v7-p-4): same recipe. The e2e shim passes EQUAL dx (⇒ nlopt_compute_rescaling returns
# identity scaling, so the bobyqa() layer matches our unscaled port); prelim/rescue shims build the stop
# struct so the stop-taking routines are per-routine diffable too.
cat > "$BUILD/bobyqa_exposed.c" <<'EOF'
/* Externalize the static functions of the reference for per-routine differential testing. */
#define static
#include "bobyqa.c"

typedef double (*crd_bobyqa_cb)(int n, const double *x);
crd_bobyqa_cb crd_bobyqa_plain_cb;
double crd_bobyqa_nlopt_adapter(unsigned n, const double *x, double *grad, void *data)
{
    (void)grad;
    (void)data;
    return crd_bobyqa_plain_cb((int)n, x);
}
double crd_bobyqa_func_adapter(int n, const double *x, void *data)
{
    (void)data;
    return crd_bobyqa_plain_cb(n, x);
}
nlopt_result crd_bobyqa_e2e(int n, int npt, double *x, const double *xl, const double *xu, double rhobeg,
                            double xtol_rel, int maxeval, double ftol_rel, double ftol_abs, crd_bobyqa_cb cb,
                            double *minf, int *nevals_out)
{
    nlopt_stopping stop;
    int nevals = 0;
    nlopt_result rc;
    int j;
    double *dxv = (double *)malloc(sizeof(double) * (size_t)n);
    for (j = 0; j < n; ++j)
        dxv[j] = rhobeg; /* equal steps => identity rescaling */
    memset(&stop, 0, sizeof(stop));
    stop.n = (unsigned)n;
    stop.nevals_p = &nevals;
    stop.maxeval = maxeval;
    stop.minf_max = -HUGE_VAL;
    stop.ftol_rel = ftol_rel;
    stop.ftol_abs = ftol_abs;
    stop.xtol_rel = xtol_rel;
    crd_bobyqa_plain_cb = cb;
    rc = bobyqa(n, npt, x, xl, xu, dxv, &stop, minf, crd_bobyqa_nlopt_adapter, NULL);
    free(dxv);
    *nevals_out = nevals;
    return rc;
}
nlopt_result crd_prelim_shim(int n, int npt, double *x, const double *xl, const double *xu, double rhobeg,
                             int maxeval, crd_bobyqa_cb cb, double *xbase, double *xpt, double *fval,
                             double *gopt, double *hq, double *pq, double *bmat, double *zmat, int ndim,
                             double *sl, double *su, int *kopt, int *nevals_out)
{
    nlopt_stopping stop;
    int nevals = 0;
    nlopt_result rc;
    memset(&stop, 0, sizeof(stop));
    stop.n = (unsigned)n;
    stop.nevals_p = &nevals;
    stop.maxeval = maxeval;
    stop.minf_max = -HUGE_VAL;
    crd_bobyqa_plain_cb = cb;
    rc = prelim_(&n, &npt, x, xl, xu, &rhobeg, &stop, crd_bobyqa_func_adapter, NULL, xbase, xpt, fval, gopt,
                 hq, pq, bmat, zmat, &ndim, sl, su, kopt);
    *nevals_out = nevals;
    return rc;
}
EOF
gcc -O2 -c "$BUILD/bobyqa_exposed.c" -o "$BUILD/bobyqa_exposed.o" \
    -I"$NLOPT/src/algs/bobyqa" -I"$NLOPT/src/util" -I"$NLOPT/src/api" \
    -I"$BUILD" -I"$BUILD/src/api"
cp "$LIB" "$BUILD/libnlopt_nobobyqa.a"
BOBJ=$(ar t "$BUILD/libnlopt_nobobyqa.a" | grep -i '^bobyqa' | head -1)
[ -n "$BOBJ" ] && ar d "$BUILD/libnlopt_nobobyqa.a" "$BOBJ"

echo "OK: oracle ready"
echo "  end-to-end lib : $LIB"
echo "  cobyla         : $BUILD/cobyla_exposed.o + $BUILD/libnlopt_nocobyla.a"
echo "  newuoa         : $BUILD/newuoa_exposed.o + $BUILD/libnlopt_nonewuoa.a"
echo "  bobyqa         : $BUILD/bobyqa_exposed.o + $BUILD/libnlopt_nobobyqa.a"
echo "  headers        : -I$NLOPT/src/util -I$BUILD/src/api"
