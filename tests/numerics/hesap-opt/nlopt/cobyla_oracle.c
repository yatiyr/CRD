/* The COBYLA oracle of cobyla_difftest: the pinned NLopt's cobyla.c with its file-scope functions made external, so
 * the harness can call trstlp on the inputs it gives Cerid's port, and an end-to-end entry at the cobyla() layer (no
 * NLopt rescaling, zero constraint tolerances: the layer minimize_cobyla mirrors). The reference source is included
 * unmodified. Its headers come first, so the static-erasing macro reaches only its own definitions; the stock cobyla
 * member of the NLopt library is never extracted, because this unit defines every symbol it would supply.
 * Built by tests/numerics/hesap-opt/CMakeLists.txt under CRD_BUILD_HESAP_VS_NLOPT. */
#include "cobyla.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define static
#include "cobyla.c"
#undef static

typedef int (*crd_calcfc_plain)(int n, int m, const double* x, double* f, double* con);

static crd_calcfc_plain crd_cobyla_callback;

static int crd_calcfc_adapter(int n, int m, double* x, double* f, double* con, func_wrap_state* state)
{
    (void)state;
    return crd_cobyla_callback(n, m, x, f, con);
}

nlopt_result crd_cobyla_e2e(int n, int m, double* x, double* minf, double rhobeg, double rhoend, int maxeval,
                            double ftol_rel, double ftol_abs, const double* lb, const double* ub, crd_calcfc_plain cb,
                            int* nevals_out)
{
    func_wrap_state state;
    nlopt_stopping stop;
    int nevals = 0;
    double* con_tol = (double*)calloc((size_t)(m + 1), sizeof(double));
    if (con_tol == NULL)
    {
        return NLOPT_OUT_OF_MEMORY;
    }
    memset(&state, 0, sizeof(state));
    memset(&stop, 0, sizeof(stop));
    stop.n = (unsigned)n;
    stop.nevals_p = &nevals;
    stop.maxeval = maxeval;
    stop.minf_max = -HUGE_VAL;
    stop.ftol_rel = ftol_rel;
    stop.ftol_abs = ftol_abs;
    state.con_tol = con_tol;
    crd_cobyla_callback = cb;
    const nlopt_result rc = cobyla(n, m, x, minf, rhobeg, rhoend, &stop, lb, ub, 0, crd_calcfc_adapter, &state);
    *nevals_out = nevals;
    free(con_tol);
    return rc;
}
