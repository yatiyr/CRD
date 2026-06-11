// bobyqa_difftest.cpp — Phase 3.1.6 v7-p-4 DIFFERENTIAL-TEST HARNESS (the L-BFGS-B/COBYLA/NEWUOA playbook).
// Layers vs the compiled NLopt reference (bobyqa_exposed.o from scripts/setup-nlopt-ref.sh):
//   1. PER-ROUTINE — update_/altmov_/trsbox_ on IDENTICAL randomized model states (bound-rich: sl/su tight
//      so the active-set paths fire), plus prelim_ through the stop-building shim — outputs BIT-EXACT.
//   2. END-TO-END — the `crd_bobyqa_e2e` shim (EQUAL dx ⇒ identity rescaling, so the bobyqa() layer matches
//      our unscaled port; xtol_rel passed so both compute rhoend identically) vs `minimize_bobyqa`:
//      bit-exact x and minf with IDENTICAL EVALUATION COUNTS. NOTE: rescue_ has no targeted per-routine diff
//      (it needs a full coherent degenerate model state); its coverage is opportunistic via e2e — honest gap.
// Dev-only, WSL, never CI/shipped. Build: the cobyla/newuoa recipe with bobyqa_exposed.o + libnlopt_nobobyqa.a.

#include <crd/hesap/opt/bobyqa.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace opt = crd::hesap::opt;
namespace bi = crd::hesap::opt::detail::bobyqa_impl;

extern "C"
{
    void update_(int* n, int* npt, double* bmat, double* zmat, int* ndim, double* vlag, double* beta, double* denom,
                 int* knew, double* w);
    void altmov_(int* n, int* npt, double* xpt, double* xopt, double* bmat, double* zmat, int* ndim, double* sl,
                 double* su, int* kopt, int* knew, double* adelt, double* xnew, double* xalt, double* alpha,
                 double* cauchy, double* glag, double* hcol, double* w);
    void trsbox_(int* n, int* npt, double* xpt, double* xopt, double* gopt, double* hq, double* pq, double* sl,
                 double* su, double* delta, double* xnew, double* d, double* gnew, double* xbdi, double* s, double* hs,
                 double* hred, double* dsq, double* crvmin);
    typedef double (*crd_bobyqa_cb)(int n, const double* x);
    int crd_prelim_shim(int n, int npt, double* x, const double* xl, const double* xu, double rhobeg, int maxeval,
                        crd_bobyqa_cb cb, double* xbase, double* xpt, double* fval, double* gopt, double* hq,
                        double* pq, double* bmat, double* zmat, int ndim, double* sl, double* su, int* kopt,
                        int* nevals_out);
    int crd_bobyqa_e2e(int n, int npt, double* x, const double* xl, const double* xu, double rhobeg, double xtol_rel,
                       int maxeval, double ftol_rel, double ftol_abs, crd_bobyqa_cb cb, double* minf, int* nevals_out);
}

namespace
{

std::mt19937_64 g_rng(0xB0BAULL);
int g_fail = 0;
int g_pass = 0;

double rr(double lo, double hi)
{
    return std::uniform_real_distribution<double>(lo, hi)(g_rng);
}

void check(bool ok, const char* what, int trial)
{
    if (ok)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("FAIL: %s (trial %d)\n", what, trial);
    }
}

bool bits_eq(const double* a, const double* b, int count)
{
    return std::memcmp(a, b, sizeof(double) * static_cast<size_t>(count)) == 0;
}

constexpr int kMaxN = 5;
constexpr int kMaxNpt = 2 * kMaxN + 1 + 6;
constexpr int kMaxDim = kMaxNpt + kMaxN;

struct ModelState
{
    int n, npt, ndim, kopt, knew;
    double xopt[kMaxN];
    double xpt[kMaxNpt * kMaxN];
    double gopt[kMaxN];
    double hq[kMaxN * (kMaxN + 1) / 2];
    double pq[kMaxNpt];
    double bmat[kMaxDim * kMaxN];
    double zmat[kMaxNpt * kMaxN];
    double sl[kMaxN];
    double su[kMaxN];

    void randomize(int n_, int npt_, bool tight_bounds)
    {
        n = n_;
        npt = npt_;
        ndim = npt + n;
        const int nptm = npt - n - 1;
        kopt = 1 + static_cast<int>(g_rng() % static_cast<unsigned>(npt));
        do
        {
            knew = 1 + static_cast<int>(g_rng() % static_cast<unsigned>(npt));
        } while (knew == kopt);
        for (int i = 0; i < n; ++i)
        {
            xopt[i] = rr(-0.5, 0.5);
            gopt[i] = rr(-2, 2);
            // Shifted bounds: tight boxes make the active-set paths fire in trsbox/altmov.
            sl[i] = tight_bounds ? xopt[i] - rr(0.05, 0.4) : -10.0;
            su[i] = tight_bounds ? xopt[i] + rr(0.05, 0.4) : 10.0;
        }
        for (int i = 0; i < npt * n; ++i)
        {
            xpt[i] = rr(-0.5, 0.5);
        }
        for (int i = 0; i < n * (n + 1) / 2; ++i)
        {
            hq[i] = rr(-1, 1);
        }
        for (int i = 0; i < npt; ++i)
        {
            pq[i] = rr(-1, 1);
        }
        for (int i = 0; i < ndim * n; ++i)
        {
            bmat[i] = rr(-1, 1);
        }
        for (int i = 0; i < npt * nptm; ++i)
        {
            zmat[i] = rr(-1, 1);
        }
    }
};

void diff_update(const ModelState& m, int trial)
{
    double bmat_o[kMaxDim * kMaxN], bmat_c[kMaxDim * kMaxN];
    double zmat_o[kMaxNpt * kMaxN], zmat_c[kMaxNpt * kMaxN];
    double vlag_o[kMaxDim], vlag_c[kMaxDim], w_o[kMaxDim], w_c[kMaxDim];
    std::memcpy(bmat_o, m.bmat, sizeof(m.bmat));
    std::memcpy(bmat_c, m.bmat, sizeof(m.bmat));
    std::memcpy(zmat_o, m.zmat, sizeof(m.zmat));
    std::memcpy(zmat_c, m.zmat, sizeof(m.zmat));
    for (int i = 0; i < m.ndim; ++i)
    {
        vlag_o[i] = vlag_c[i] = rr(-1, 1);
    }
    const double beta = rr(-2, 2);
    const double denom = rr(0.1, 3.0); // BOBYQA's denom is positive
    double beta_o = beta, beta_c = beta, denom_o = denom;
    int n = m.n, npt = m.npt, ndim = m.ndim, knew = m.knew;
    update_(&n, &npt, bmat_o, zmat_o, &ndim, vlag_o, &beta_o, &denom_o, &knew, w_o);
    bi::update<double>(&n, &npt, bmat_c, zmat_c, &ndim, vlag_c, &beta_c, &denom, &knew, w_c);
    check(bits_eq(bmat_o, bmat_c, ndim * n), "update bmat bit-exact", trial);
    check(bits_eq(zmat_o, zmat_c, npt * (npt - n - 1)), "update zmat bit-exact", trial);
    check(bits_eq(vlag_o, vlag_c, ndim), "update vlag bit-exact", trial);
}

void diff_altmov(const ModelState& m, double adelt, int trial)
{
    double xpt_o[kMaxNpt * kMaxN], xpt_c[kMaxNpt * kMaxN], xopt_o[kMaxN], xopt_c[kMaxN];
    double bmat_o[kMaxDim * kMaxN], bmat_c[kMaxDim * kMaxN], zmat_o[kMaxNpt * kMaxN], zmat_c[kMaxNpt * kMaxN];
    double sl_o[kMaxN], sl_c[kMaxN], su_o[kMaxN], su_c[kMaxN];
    double xnew_o[kMaxN], xnew_c[kMaxN], xalt_o[kMaxN], xalt_c[kMaxN];
    double glag_o[kMaxN], glag_c[kMaxN], hcol_o[kMaxNpt], hcol_c[kMaxNpt], w_o[2 * kMaxN], w_c[2 * kMaxN];
    double alpha_o = 0, alpha_c = 0, cauchy_o = 0, cauchy_c = 0;
    std::memcpy(xpt_o, m.xpt, sizeof(m.xpt));
    std::memcpy(xpt_c, m.xpt, sizeof(m.xpt));
    std::memcpy(xopt_o, m.xopt, sizeof(m.xopt));
    std::memcpy(xopt_c, m.xopt, sizeof(m.xopt));
    std::memcpy(bmat_o, m.bmat, sizeof(m.bmat));
    std::memcpy(bmat_c, m.bmat, sizeof(m.bmat));
    std::memcpy(zmat_o, m.zmat, sizeof(m.zmat));
    std::memcpy(zmat_c, m.zmat, sizeof(m.zmat));
    std::memcpy(sl_o, m.sl, sizeof(m.sl));
    std::memcpy(sl_c, m.sl, sizeof(m.sl));
    std::memcpy(su_o, m.su, sizeof(m.su));
    std::memcpy(su_c, m.su, sizeof(m.su));
    int n = m.n, npt = m.npt, ndim = m.ndim, kopt = m.kopt, knew = m.knew;
    double adelt_o = adelt, adelt_c = adelt;
    altmov_(&n, &npt, xpt_o, xopt_o, bmat_o, zmat_o, &ndim, sl_o, su_o, &kopt, &knew, &adelt_o, xnew_o, xalt_o,
            &alpha_o, &cauchy_o, glag_o, hcol_o, w_o);
    bi::altmov<double>(&n, &npt, xpt_c, xopt_c, bmat_c, zmat_c, &ndim, sl_c, su_c, &kopt, &knew, &adelt_c, xnew_c,
                       xalt_c, &alpha_c, &cauchy_c, glag_c, hcol_c, w_c);
    check(bits_eq(xnew_o, xnew_c, n), "altmov xnew bit-exact", trial);
    check(bits_eq(xalt_o, xalt_c, n), "altmov xalt bit-exact", trial);
    check(std::memcmp(&alpha_o, &alpha_c, sizeof(double)) == 0, "altmov alpha bit-exact", trial);
    check(std::memcmp(&cauchy_o, &cauchy_c, sizeof(double)) == 0, "altmov cauchy bit-exact", trial);
}

void diff_trsbox(const ModelState& m, double delta, int trial)
{
    double xpt_o[kMaxNpt * kMaxN], xpt_c[kMaxNpt * kMaxN], xopt_o[kMaxN], xopt_c[kMaxN];
    double gopt_o[kMaxN], gopt_c[kMaxN], hq_o[16], hq_c[16], pq_o[kMaxNpt], pq_c[kMaxNpt];
    double sl_o[kMaxN], sl_c[kMaxN], su_o[kMaxN], su_c[kMaxN];
    double xnew_o[kMaxN], xnew_c[kMaxN], d_o[kMaxN], d_c[kMaxN], gnew_o[kMaxN], gnew_c[kMaxN];
    double xbdi_o[kMaxN], xbdi_c[kMaxN], s_o[kMaxN], s_c[kMaxN], hs_o[kMaxN], hs_c[kMaxN], hred_o[kMaxN], hred_c[kMaxN];
    double dsq_o = 0, dsq_c = 0, crv_o = 0, crv_c = 0;
    std::memcpy(xpt_o, m.xpt, sizeof(m.xpt));
    std::memcpy(xpt_c, m.xpt, sizeof(m.xpt));
    std::memcpy(xopt_o, m.xopt, sizeof(m.xopt));
    std::memcpy(xopt_c, m.xopt, sizeof(m.xopt));
    std::memcpy(gopt_o, m.gopt, sizeof(m.gopt));
    std::memcpy(gopt_c, m.gopt, sizeof(m.gopt));
    std::memcpy(hq_o, m.hq, sizeof(m.hq));
    std::memcpy(hq_c, m.hq, sizeof(m.hq));
    std::memcpy(pq_o, m.pq, sizeof(m.pq));
    std::memcpy(pq_c, m.pq, sizeof(m.pq));
    std::memcpy(sl_o, m.sl, sizeof(m.sl));
    std::memcpy(sl_c, m.sl, sizeof(m.sl));
    std::memcpy(su_o, m.su, sizeof(m.su));
    std::memcpy(su_c, m.su, sizeof(m.su));
    int n = m.n, npt = m.npt;
    double del_o = delta, del_c = delta;
    trsbox_(&n, &npt, xpt_o, xopt_o, gopt_o, hq_o, pq_o, sl_o, su_o, &del_o, xnew_o, d_o, gnew_o, xbdi_o, s_o, hs_o,
            hred_o, &dsq_o, &crv_o);
    bi::trsbox<double>(&n, &npt, xpt_c, xopt_c, gopt_c, hq_c, pq_c, sl_c, su_c, &del_c, xnew_c, d_c, gnew_c, xbdi_c,
                       s_c, hs_c, hred_c, &dsq_c, &crv_c);
    check(bits_eq(xnew_o, xnew_c, n), "trsbox xnew bit-exact", trial);
    check(bits_eq(d_o, d_c, n), "trsbox d bit-exact", trial);
    check(bits_eq(xbdi_o, xbdi_c, n), "trsbox xbdi bit-exact", trial);
    check(std::memcmp(&dsq_o, &dsq_c, sizeof(double)) == 0, "trsbox dsq bit-exact", trial);
    check(std::memcmp(&crv_o, &crv_c, sizeof(double)) == 0, "trsbox crvmin bit-exact", trial);
}

// ----------------------------------------------------------------------------- prelim (shim) + end-to-end
double cb_rosen(int, const double* x)
{
    const double a = 1.0 - x[0];
    const double b = x[1] - x[0] * x[0];
    return a * a + 100.0 * b * b;
}

double cb_quad3(int, const double* x)
{
    const double c[3] = {1.5, -0.5, 2.0};
    double acc = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double d = x[i] - c[i];
        acc += (1.0 + i) * d * d;
    }
    return acc;
}

double cb_quad2_outside(int, const double* x)
{
    const double c[2] = {1.5, -0.5};
    double acc = 0.0;
    for (int i = 0; i < 2; ++i)
    {
        const double d = x[i] - c[i];
        acc += (1.0 + i) * d * d;
    }
    return acc;
}

void diff_prelim(int n, int npt, const double* x0, const double* xl, const double* xu, double rhobeg,
                 double (*cb)(int, const double*), int trial)
{
    const int ndim = npt + n;
    // Build SL/SU like the public driver / reference bobyqa() does, identically for both sides.
    double sl[kMaxN], su[kMaxN], x_o[kMaxN], x_c[kMaxN];
    for (int j = 0; j < n; ++j)
    {
        sl[j] = xl[j] - x0[j];
        su[j] = xu[j] - x0[j];
        x_o[j] = x_c[j] = x0[j];
    }
    double xbase_o[kMaxN], xbase_c[kMaxN], xpt_o[kMaxNpt * kMaxN], xpt_c[kMaxNpt * kMaxN];
    double fval_o[kMaxNpt], fval_c[kMaxNpt], gopt_o[kMaxN], gopt_c[kMaxN], hq_o[16], hq_c[16];
    double pq_o[kMaxNpt], pq_c[kMaxNpt], bmat_o[kMaxDim * kMaxN], bmat_c[kMaxDim * kMaxN];
    double zmat_o[kMaxNpt * kMaxN], zmat_c[kMaxNpt * kMaxN];
    double sl_o[kMaxN], sl_c[kMaxN], su_o[kMaxN], su_c[kMaxN];
    std::memcpy(sl_o, sl, sizeof(sl));
    std::memcpy(sl_c, sl, sizeof(sl));
    std::memcpy(su_o, su, sizeof(su));
    std::memcpy(su_c, su, sizeof(su));
    int kopt_o = 0, kopt_c = 0, nevals_o = 0;
    const int rc_o = crd_prelim_shim(n, npt, x_o, xl, xu, rhobeg, 100000, cb, xbase_o, xpt_o, fval_o, gopt_o, hq_o,
                                     pq_o, bmat_o, zmat_o, ndim, sl_o, su_o, &kopt_o, &nevals_o);
    bi::Stop<double> stop;
    stop.maxeval = 100000;
    auto calfun = [&](int cn, const double* xx) -> double
    {
        return cb(cn, xx);
    };
    const bi::Rc rc_c = bi::prelim<double>(&n, &npt, x_c, xl, xu, &rhobeg, &stop, calfun, xbase_c, xpt_c, fval_c,
                                           gopt_c, hq_c, pq_c, bmat_c, zmat_c, &ndim, sl_c, su_c, &kopt_c);
    check(static_cast<int>(rc_c) == rc_o, "prelim rc", trial);
    check(kopt_c == kopt_o, "prelim kopt", trial);
    check(stop.nevals == nevals_o, "prelim nevals", trial);
    check(bits_eq(xpt_o, xpt_c, npt * n), "prelim xpt bit-exact", trial);
    check(bits_eq(fval_o, fval_c, npt), "prelim fval bit-exact", trial);
    check(bits_eq(gopt_o, gopt_c, n), "prelim gopt bit-exact", trial);
    check(bits_eq(bmat_o, bmat_c, ndim * n), "prelim bmat bit-exact", trial);
    check(bits_eq(zmat_o, zmat_c, npt * (npt - n - 1)), "prelim zmat bit-exact", trial);
}

class CbObj final : public opt::Objective<double>
{
public:
    CbObj(double (*cb)(int, const double*), int n) noexcept : Objective<double>(false, false), m_cb(cb), m_n(n) {}
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        return m_cb(static_cast<int>(x.size()), x.data());
    }
    [[nodiscard]] crd::usize n() const noexcept override { return static_cast<crd::usize>(m_n); }

private:
    double (*m_cb)(int, const double*);
    int m_n;
};

void diff_e2e(const char* name, double (*cb)(int, const double*), int n, int npt, const double* x0, const double* xl,
              const double* xu, double rhobeg, double rhoend, crd::memory::IAllocator* alloc)
{
    double x_o[8];
    std::memcpy(x_o, x0, sizeof(double) * static_cast<size_t>(n));
    double minf_o = 0.0;
    int nevals_o = 0;
    const double xtol_rel = rhoend / rhobeg;
    const int rc_o = crd_bobyqa_e2e(n, npt, x_o, xl, xu, rhobeg, xtol_rel, 100000, 0.0, 0.0, cb, &minf_o, &nevals_o);

    const CbObj obj(cb, n);
    opt::BobyqaOptions<double> bo;
    bo.rhobeg = rhobeg;
    bo.rhoend = rhoend;
    bo.npt = static_cast<crd::usize>(npt);
    bo.max_evals = 100000;
    const opt::OptResult<double> r =
        opt::minimize_bobyqa<double>(obj, {x0, static_cast<crd::usize>(n)}, {xl, static_cast<crd::usize>(n)},
                                     {xu, static_cast<crd::usize>(n)}, alloc, bo);

    char what[128];
    std::snprintf(what, sizeof(what), "e2e %s rc (oracle %d)", name, rc_o);
    check(rc_o >= 1, what, 0);
    std::snprintf(what, sizeof(what), "e2e %s nevals (oracle %d, cerid %u)", name, nevals_o,
                  static_cast<unsigned>(r.fn_evals));
    check(static_cast<int>(r.fn_evals) == nevals_o, what, 0);
    std::snprintf(what, sizeof(what), "e2e %s minf bit-exact (oracle %.17g, cerid %.17g)", name, minf_o, r.fx);
    check(std::memcmp(&minf_o, &r.fx, sizeof(double)) == 0, what, 0);
    for (int i = 0; i < n; ++i)
    {
        std::snprintf(what, sizeof(what), "e2e %s x[%d] bit-exact", name, i);
        check(std::memcmp(&x_o[i], &r.x[static_cast<crd::usize>(i)], sizeof(double)) == 0, what, 0);
    }
}

} // namespace

int main()
{
    // Per-routine randomized sweeps (half the trials with TIGHT bounds to fire the active-set paths).
    for (int trial = 0; trial < 250; ++trial)
    {
        const int n = 2 + static_cast<int>(g_rng() % 4); // 2..5
        const int npt_min = n + 2;
        const int npt_max = 2 * n + 1;
        const int npt = npt_min + static_cast<int>(g_rng() % static_cast<unsigned>(npt_max - npt_min + 1));
        ModelState m;
        m.randomize(n, npt, trial % 2 == 0);
        const double delta = trial % 3 == 0 ? 0.05 : (trial % 3 == 1 ? 0.3 : 1.5);
        diff_update(m, trial);
        diff_altmov(m, delta, trial);
        diff_trsbox(m, delta, trial);
    }

    // prelim through the shim (bound-aware init: interior, near-bound, and at-bound starts).
    {
        const double xl3[] = {-5.0, -5.0, -5.0};
        const double xu3[] = {5.0, 5.0, 5.0};
        const double x0a[] = {0.0, 0.0, 0.0};
        diff_prelim(3, 7, x0a, xl3, xu3, 0.5, cb_quad3, -1);
        const double x0b[] = {-4.8, 4.9, 0.0}; // near the bounds: the step flip/shrink paths
        diff_prelim(3, 10, x0b, xl3, xu3, 0.5, cb_quad3, -2);
        const double xl2[] = {-2.0, -2.0};
        const double xu2[] = {1.0, 2.0};
        const double x0c[] = {1.0, -2.0}; // exactly AT bounds
        diff_prelim(2, 6, x0c, xl2, xu2, 0.25, cb_quad2_outside, -3);
    }

    // End-to-end shared problems.
    crd::memory::TlsfAllocator alloc(1U << 24);
    const double x0_rosen[] = {-1.2, 1.0};
    const double lo2[] = {-2.0, -2.0};
    const double up2[] = {2.0, 2.0};
    const double up2_pin[] = {1.0, 2.0};
    const double x0_zero2[] = {0.0, 0.0};
    const double x0_zero3[] = {0.0, 0.0, 0.0};
    const double lo3[] = {-5.0, -5.0, -5.0};
    const double up3[] = {5.0, 5.0, 5.0};
    diff_e2e("rosen-box", cb_rosen, 2, 5, x0_rosen, lo2, up2, 0.5, 1e-10, &alloc);
    diff_e2e("rosen-box npt=6", cb_rosen, 2, 6, x0_rosen, lo2, up2, 1.0, 1e-8, &alloc);
    diff_e2e("quad3 interior full-npt", cb_quad3, 3, 10, x0_zero3, lo3, up3, 0.5, 1e-10, &alloc);
    diff_e2e("quad2 bound-pinned", cb_quad2_outside, 2, 5, x0_zero2, lo2, up2_pin, 0.25, 1e-10, &alloc);

    std::printf("bobyqa_difftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
