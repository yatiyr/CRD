/* The NEWUOA oracle of newuoa_difftest: the pinned NLopt's newuoa.c with its file-scope functions made external, so
 * the harness can call trsapp_, biglag_, bigden_ and update_ on the inputs it gives Cerid's port, and an end-to-end
 * entry at the newuoa() layer with no bounds (the classic unconstrained algorithm, the port's scope) that passes
 * xtol_rel, so both sides compute rhoend = xtol_rel * rhobeg. The reference source is included unmodified. Its
 * headers come first, so the static-erasing macro reaches only its own definitions; the stock newuoa member of the
 * NLopt library is never extracted, because this unit defines every symbol it would supply.
 * Built by tests/numerics/hesap-opt/CMakeLists.txt under CRD_BUILD_HESAP_VS_NLOPT. */
#include "newuoa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The oracle takes its elementary functions from crd-math, as the port does (newuoa_difftest.cpp defines these three),
 * so the comparison judges the translation and not two libraries' last places: glibc's sin, cos and atan are not
 * correctly rounded, and one angle a unit apart carries through the rest of a step. sqrt and fabs are exact in both. */
double crd_oracle_sin(double x);
double crd_oracle_cos(double x);
double crd_oracle_atan(double x);
#define sin crd_oracle_sin
#define cos crd_oracle_cos
#define atan crd_oracle_atan

#define static
#include "newuoa.c"
#undef static

typedef double (*crd_newuoa_cb)(int n, const double* x);

static crd_newuoa_cb crd_newuoa_callback;

static double crd_newuoa_adapter(int n, const double* x, void* data)
{
    (void)data;
    return crd_newuoa_callback(n, x);
}

nlopt_result crd_newuoa_e2e(int n, int npt, double* x, double* minf, double rhobeg, double xtol_rel, int maxeval,
                            double ftol_rel, double ftol_abs, crd_newuoa_cb cb, int* nevals_out)
{
    nlopt_stopping stop;
    int nevals = 0;
    memset(&stop, 0, sizeof(stop));
    stop.n = (unsigned)n;
    stop.nevals_p = &nevals;
    stop.maxeval = maxeval;
    stop.minf_max = -HUGE_VAL;
    stop.ftol_rel = ftol_rel;
    stop.ftol_abs = ftol_abs;
    stop.xtol_rel = xtol_rel;
    crd_newuoa_callback = cb;
    const nlopt_result rc = newuoa(n, npt, x, NULL, NULL, rhobeg, &stop, minf, crd_newuoa_adapter, NULL);
    *nevals_out = nevals;
    return rc;
}
