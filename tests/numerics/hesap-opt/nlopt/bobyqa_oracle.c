/* The BOBYQA oracle of bobyqa_difftest: the pinned NLopt's bobyqa.c with its file-scope functions made external, so
 * the harness can call update_, altmov_ and trsbox_ on the inputs it gives Cerid's port; prelim_ through an entry that
 * builds the stopping record it reads; and an end-to-end entry at the bobyqa() layer with equal initial steps, which
 * make NLopt's rescaling the identity (the layer minimize_bobyqa mirrors), passing xtol_rel so both sides compute the
 * same rhoend. The reference source is included unmodified. Its headers come first, so the static-erasing macro
 * reaches only its own definitions; the stock bobyqa member of the NLopt library is never extracted, because this unit
 * defines every symbol it would supply. Built by tests/numerics/hesap-opt/CMakeLists.txt under
 * CRD_BUILD_HESAP_VS_NLOPT. */
#include "bobyqa.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define static
#include "bobyqa.c"
#undef static

typedef double (*crd_bobyqa_cb)(int n, const double* x);

static crd_bobyqa_cb crd_bobyqa_callback;

static double crd_bobyqa_nlopt_adapter(unsigned n, const double* x, double* grad, void* data)
{
    (void)grad;
    (void)data;
    return crd_bobyqa_callback((int)n, x);
}

static double crd_bobyqa_func_adapter(int n, const double* x, void* data)
{
    (void)data;
    return crd_bobyqa_callback(n, x);
}

static void crd_bobyqa_stop_init(nlopt_stopping* stop, int n, int maxeval, int* nevals)
{
    memset(stop, 0, sizeof(*stop));
    stop->n = (unsigned)n;
    stop->nevals_p = nevals;
    stop->maxeval = maxeval;
    stop->minf_max = -HUGE_VAL;
}

nlopt_result crd_bobyqa_e2e(int n, int npt, double* x, const double* xl, const double* xu, double rhobeg,
                            double xtol_rel, int maxeval, double ftol_rel, double ftol_abs, crd_bobyqa_cb cb,
                            double* minf, int* nevals_out)
{
    nlopt_stopping stop;
    int nevals = 0;
    double* dx = (double*)malloc(sizeof(double) * (size_t)n);
    if (dx == NULL)
    {
        return NLOPT_OUT_OF_MEMORY;
    }
    for (int j = 0; j < n; ++j)
    {
        dx[j] = rhobeg;
    }
    crd_bobyqa_stop_init(&stop, n, maxeval, &nevals);
    stop.ftol_rel = ftol_rel;
    stop.ftol_abs = ftol_abs;
    stop.xtol_rel = xtol_rel;
    crd_bobyqa_callback = cb;
    const nlopt_result rc = bobyqa(n, npt, x, xl, xu, dx, &stop, minf, crd_bobyqa_nlopt_adapter, NULL);
    free(dx);
    *nevals_out = nevals;
    return rc;
}

nlopt_result crd_prelim_shim(int n, int npt, double* x, const double* xl, const double* xu, double rhobeg, int maxeval,
                             crd_bobyqa_cb cb, double* xbase, double* xpt, double* fval, double* gopt, double* hq,
                             double* pq, double* bmat, double* zmat, int ndim, double* sl, double* su, int* kopt,
                             int* nevals_out)
{
    nlopt_stopping stop;
    int nevals = 0;
    crd_bobyqa_stop_init(&stop, n, maxeval, &nevals);
    crd_bobyqa_callback = cb;
    const nlopt_result rc = prelim_(&n, &npt, x, xl, xu, &rhobeg, &stop, crd_bobyqa_func_adapter, NULL, xbase, xpt,
                                    fval, gopt, hq, pq, bmat, zmat, &ndim, sl, su, kopt);
    *nevals_out = nevals;
    return rc;
}
