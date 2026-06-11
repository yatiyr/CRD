// newuoa_difftest.cpp — Phase 3.1.6 v7-p-3 DIFFERENTIAL-TEST HARNESS (the L-BFGS-B/COBYLA playbook). Two
// layers vs the compiled NLopt reference (newuoa_exposed.o from scripts/setup-nlopt-ref.sh):
//   1. PER-ROUTINE — trsapp_/biglag_/bigden_/update_ on IDENTICAL inputs (randomized model states exercise
//      the same arithmetic; targeted seeds hit the jl==1/jl>1 + idz branches of update_), outputs compared
//      BIT-EXACT: step/d/vlag/w/beta/alpha/crvmin reals, bmat/zmat matrices, idz ints, return codes.
//   2. END-TO-END — the `crd_newuoa_e2e` shim (NULL bounds = the classic unconstrained scope; xtol_rel passed
//      so both sides compute rhoend = xtol_rel*rhobeg identically) vs `minimize_newuoa`: bit-exact x and
//      minf with IDENTICAL EVALUATION COUNTS.
// Dev-only, WSL, never CI/shipped. Build (after scripts/setup-nlopt-ref.sh; from the repo root): same recipe
// as cobyla_difftest with newuoa_exposed.o + libnlopt_nonewuoa.a.

#include <crd/hesap/opt/newuoa.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace opt = crd::hesap::opt;
namespace ni = crd::hesap::opt::detail::newuoa_impl;

extern "C"
{
    int trsapp_(int* n, int* npt, double* xopt, double* xpt, double* gq, double* hq, double* pq, double* delta,
                double* step, double* d, double* g, double* hd, double* hs, double* crvmin, const double* xbase,
                const double* lb, const double* ub);
    int bigden_(int* n, int* npt, double* xopt, double* xpt, double* bmat, double* zmat, int* idz, int* ndim, int* kopt,
                int* knew, double* d, double* w, double* vlag, double* beta, double* s, double* wvec, double* prod,
                const double* xbase, const double* lb, const double* ub);
    int biglag_(int* n, int* npt, double* xopt, double* xpt, double* bmat, double* zmat, int* idz, int* ndim, int* knew,
                double* delta, double* d, double* alpha, double* hcol, double* gc, double* gd, double* s, double* w,
                const double* xbase, const double* lb, const double* ub);
    void update_(int* n, int* npt, double* bmat, double* zmat, int* idz, int* ndim, double* vlag, double* beta,
                 int* knew, double* w);
    typedef double (*crd_newuoa_cb)(int n, const double* x);
    int crd_newuoa_e2e(int n, int npt, double* x, double* minf, double rhobeg, double xtol_rel, int maxeval,
                       double ftol_rel, double ftol_abs, crd_newuoa_cb cb, int* nevals_out);
}

namespace
{

std::mt19937_64 g_rng(0x9E10AULL);
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
constexpr int kMaxNpt = 2 * kMaxN + 1 + 6; // room beyond 2n+1
constexpr int kMaxDim = kMaxNpt + kMaxN;

struct ModelState // a randomized-but-shared model state marshalled into both implementations
{
    int n, npt, ndim, idz, kopt, knew;
    double xopt[kMaxN];
    double xpt[kMaxNpt * kMaxN];
    double gq[kMaxN];
    double hq[kMaxN * (kMaxN + 1) / 2];
    double pq[kMaxNpt];
    double bmat[kMaxDim * kMaxN];
    double zmat[kMaxNpt * kMaxN];

    void randomize(int n_, int npt_)
    {
        n = n_;
        npt = npt_;
        ndim = npt + n;
        const int nptm = npt - n - 1;
        idz = 1 + static_cast<int>(g_rng() % static_cast<unsigned>(nptm > 1 ? nptm : 1));
        kopt = 1 + static_cast<int>(g_rng() % static_cast<unsigned>(npt));
        do
        {
            knew = 1 + static_cast<int>(g_rng() % static_cast<unsigned>(npt));
        } while (knew == kopt);
        for (int i = 0; i < n; ++i)
        {
            xopt[i] = rr(-1, 1);
            gq[i] = rr(-2, 2);
        }
        for (int i = 0; i < npt * n; ++i)
        {
            xpt[i] = rr(-1, 1);
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

void diff_trsapp(const ModelState& m, double delta, int trial)
{
    double xpt_o[kMaxNpt * kMaxN], xpt_c[kMaxNpt * kMaxN];
    double gq_o[kMaxN], gq_c[kMaxN], hq_o[16], hq_c[16], pq_o[kMaxNpt], pq_c[kMaxNpt];
    double xopt_o[kMaxN], xopt_c[kMaxN];
    double step_o[kMaxN], step_c[kMaxN], d_o[kMaxN], d_c[kMaxN], g_o[kMaxN], g_c[kMaxN];
    double hd_o[kMaxN], hd_c[kMaxN], hs_o[kMaxN], hs_c[kMaxN];
    double crv_o = 0, crv_c = 0;
    std::memcpy(xpt_o, m.xpt, sizeof(m.xpt));
    std::memcpy(xpt_c, m.xpt, sizeof(m.xpt));
    std::memcpy(gq_o, m.gq, sizeof(m.gq));
    std::memcpy(gq_c, m.gq, sizeof(m.gq));
    std::memcpy(hq_o, m.hq, sizeof(m.hq));
    std::memcpy(hq_c, m.hq, sizeof(m.hq));
    std::memcpy(pq_o, m.pq, sizeof(m.pq));
    std::memcpy(pq_c, m.pq, sizeof(m.pq));
    std::memcpy(xopt_o, m.xopt, sizeof(m.xopt));
    std::memcpy(xopt_c, m.xopt, sizeof(m.xopt));

    int n = m.n, npt = m.npt;
    double del_o = delta, del_c = delta;
    const int rc_o = trsapp_(&n, &npt, xopt_o, xpt_o, gq_o, hq_o, pq_o, &del_o, step_o, d_o, g_o, hd_o, hs_o, &crv_o,
                             nullptr, nullptr, nullptr);
    const ni::Rc rc_c =
        ni::trsapp<double>(&n, &npt, xopt_c, xpt_c, gq_c, hq_c, pq_c, &del_c, step_c, d_c, g_c, hd_c, hs_c, &crv_c);
    check(static_cast<int>(rc_c) == rc_o, "trsapp rc", trial);
    check(bits_eq(step_o, step_c, n), "trsapp step bit-exact", trial);
    check(std::memcmp(&crv_o, &crv_c, sizeof(double)) == 0, "trsapp crvmin bit-exact", trial);
}

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
    double beta = rr(-2, 2);
    double beta_o = beta, beta_c = beta;
    int n = m.n, npt = m.npt, ndim = m.ndim, knew = m.knew;
    int idz_o = m.idz, idz_c = m.idz;
    update_(&n, &npt, bmat_o, zmat_o, &idz_o, &ndim, vlag_o, &beta_o, &knew, w_o);
    ni::update<double>(&n, &npt, bmat_c, zmat_c, &idz_c, &ndim, vlag_c, &beta_c, &knew, w_c);
    check(idz_c == idz_o, "update idz", trial);
    check(bits_eq(bmat_o, bmat_c, ndim * n), "update bmat bit-exact", trial);
    check(bits_eq(zmat_o, zmat_c, npt * (npt - n - 1)), "update zmat bit-exact", trial);
    check(bits_eq(vlag_o, vlag_c, ndim), "update vlag bit-exact", trial);
}

void diff_biglag(const ModelState& m, double delta, int trial)
{
    double xopt_o[kMaxN], xopt_c[kMaxN], xpt_o[kMaxNpt * kMaxN], xpt_c[kMaxNpt * kMaxN];
    double bmat_o[kMaxDim * kMaxN], bmat_c[kMaxDim * kMaxN], zmat_o[kMaxNpt * kMaxN], zmat_c[kMaxNpt * kMaxN];
    double d_o[kMaxN], d_c[kMaxN], hcol_o[kMaxNpt], hcol_c[kMaxNpt], gc_o[kMaxN], gc_c[kMaxN];
    double gd_o[kMaxN], gd_c[kMaxN], s_o[kMaxN], s_c[kMaxN], w_o[2 * kMaxDim], w_c[2 * kMaxDim];
    double alpha_o = 0, alpha_c = 0;
    std::memcpy(xopt_o, m.xopt, sizeof(m.xopt));
    std::memcpy(xopt_c, m.xopt, sizeof(m.xopt));
    std::memcpy(xpt_o, m.xpt, sizeof(m.xpt));
    std::memcpy(xpt_c, m.xpt, sizeof(m.xpt));
    std::memcpy(bmat_o, m.bmat, sizeof(m.bmat));
    std::memcpy(bmat_c, m.bmat, sizeof(m.bmat));
    std::memcpy(zmat_o, m.zmat, sizeof(m.zmat));
    std::memcpy(zmat_c, m.zmat, sizeof(m.zmat));
    for (int i = 0; i < m.n; ++i)
    {
        d_o[i] = d_c[i] = rr(-1, 1);
    }
    int n = m.n, npt = m.npt, ndim = m.ndim, knew = m.knew;
    int idz_o = m.idz, idz_c = m.idz;
    double del_o = delta, del_c = delta;
    const int rc_o = biglag_(&n, &npt, xopt_o, xpt_o, bmat_o, zmat_o, &idz_o, &ndim, &knew, &del_o, d_o, &alpha_o,
                             hcol_o, gc_o, gd_o, s_o, w_o, nullptr, nullptr, nullptr);
    const ni::Rc rc_c = ni::biglag<double>(&n, &npt, xopt_c, xpt_c, bmat_c, zmat_c, &idz_c, &ndim, &knew, &del_c, d_c,
                                           &alpha_c, hcol_c, gc_c, gd_c, s_c, w_c);
    check(static_cast<int>(rc_c) == rc_o, "biglag rc", trial);
    check(bits_eq(d_o, d_c, n), "biglag d bit-exact", trial);
    check(std::memcmp(&alpha_o, &alpha_c, sizeof(double)) == 0, "biglag alpha bit-exact", trial);
}

void diff_bigden(const ModelState& m, int trial)
{
    double xopt_o[kMaxN], xopt_c[kMaxN], xpt_o[kMaxNpt * kMaxN], xpt_c[kMaxNpt * kMaxN];
    double bmat_o[kMaxDim * kMaxN], bmat_c[kMaxDim * kMaxN], zmat_o[kMaxNpt * kMaxN], zmat_c[kMaxNpt * kMaxN];
    double d_o[kMaxN], d_c[kMaxN], w_o[2 * kMaxDim], w_c[2 * kMaxDim], vlag_o[kMaxDim], vlag_c[kMaxDim];
    double s_o[kMaxN], s_c[kMaxN], wvec_o[kMaxDim * 5], wvec_c[kMaxDim * 5], prod_o[kMaxDim * 5], prod_c[kMaxDim * 5];
    double beta_o = 0, beta_c = 0;
    std::memcpy(xopt_o, m.xopt, sizeof(m.xopt));
    std::memcpy(xopt_c, m.xopt, sizeof(m.xopt));
    std::memcpy(xpt_o, m.xpt, sizeof(m.xpt));
    std::memcpy(xpt_c, m.xpt, sizeof(m.xpt));
    std::memcpy(bmat_o, m.bmat, sizeof(m.bmat));
    std::memcpy(bmat_c, m.bmat, sizeof(m.bmat));
    std::memcpy(zmat_o, m.zmat, sizeof(m.zmat));
    std::memcpy(zmat_c, m.zmat, sizeof(m.zmat));
    for (int i = 0; i < m.n; ++i)
    {
        d_o[i] = d_c[i] = rr(-0.5, 0.5);
    }
    int n = m.n, npt = m.npt, ndim = m.ndim, kopt = m.kopt, knew = m.knew;
    int idz_o = m.idz, idz_c = m.idz;
    const int rc_o = bigden_(&n, &npt, xopt_o, xpt_o, bmat_o, zmat_o, &idz_o, &ndim, &kopt, &knew, d_o, w_o, vlag_o,
                             &beta_o, s_o, wvec_o, prod_o, nullptr, nullptr, nullptr);
    const ni::Rc rc_c = ni::bigden<double>(&n, &npt, xopt_c, xpt_c, bmat_c, zmat_c, &idz_c, &ndim, &kopt, &knew, d_c,
                                           w_c, vlag_c, &beta_c, s_c, wvec_c, prod_c);
    check(static_cast<int>(rc_c) == rc_o, "bigden rc", trial);
    if (rc_o == 1) // only compare full outputs on the success path (roundoff exits may bail mid-fill)
    {
        check(bits_eq(d_o, d_c, n), "bigden d bit-exact", trial);
        check(std::memcmp(&beta_o, &beta_c, sizeof(double)) == 0, "bigden beta bit-exact", trial);
        check(bits_eq(vlag_o, vlag_c, ndim), "bigden vlag bit-exact", trial);
        check(bits_eq(w_o, w_c, ndim), "bigden w bit-exact", trial);
    }
}

// ----------------------------------------------------------------------------- end-to-end shared problems
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

double cb_sphere4(int, const double* x)
{
    const double c[4] = {0.3, -1.2, 0.8, 2.1};
    double acc = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const double d = x[i] - c[i];
        acc += d * d;
    }
    return acc;
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

void diff_e2e(const char* name, double (*cb)(int, const double*), int n, int npt, const double* x0, double rhobeg,
              double rhoend, crd::memory::IAllocator* alloc)
{
    double x_o[8];
    std::memcpy(x_o, x0, sizeof(double) * static_cast<size_t>(n));
    double minf_o = 0.0;
    int nevals_o = 0;
    const double xtol_rel = rhoend / rhobeg;
    const int rc_o = crd_newuoa_e2e(n, npt, x_o, &minf_o, rhobeg, xtol_rel, 100000, 0.0, 0.0, cb, &nevals_o);

    const CbObj obj(cb, n);
    opt::NewuoaOptions<double> no;
    no.rhobeg = rhobeg;
    no.rhoend = rhoend;
    no.npt = static_cast<crd::usize>(npt);
    no.max_evals = 100000;
    const opt::OptResult<double> r = opt::minimize_newuoa<double>(obj, {x0, static_cast<crd::usize>(n)}, alloc, no);

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
    // Per-routine randomized sweeps over shared model states.
    for (int trial = 0; trial < 250; ++trial)
    {
        const int n = 2 + static_cast<int>(g_rng() % 4); // 2..5
        const int npt_min = n + 2;
        const int npt_max = 2 * n + 1;
        const int npt = npt_min + static_cast<int>(g_rng() % static_cast<unsigned>(npt_max - npt_min + 1));
        ModelState m;
        m.randomize(n, npt);
        const double delta = trial % 3 == 0 ? 0.05 : (trial % 3 == 1 ? 0.5 : 2.0);
        diff_trsapp(m, delta, trial);
        diff_update(m, trial);
        diff_biglag(m, delta, trial);
        diff_bigden(m, trial);
    }

    // End-to-end shared problems (NULL bounds — the classic unconstrained scope both sides).
    crd::memory::TlsfAllocator alloc(1U << 24);
    const double x0_rosen[] = {-1.2, 1.0};
    const double x0_zero3[] = {0.0, 0.0, 0.0};
    const double x0_zero4[] = {0.0, 0.0, 0.0, 0.0};
    diff_e2e("rosen npt=5", cb_rosen, 2, 5, x0_rosen, 0.5, 1e-10, &alloc);
    diff_e2e("rosen npt=6", cb_rosen, 2, 6, x0_rosen, 1.0, 1e-8, &alloc);
    diff_e2e("quad3 full-npt", cb_quad3, 3, 10, x0_zero3, 0.5, 1e-10, &alloc);
    diff_e2e("sphere4 npt=9", cb_sphere4, 4, 9, x0_zero4, 1.0, 1e-9, &alloc);

    std::printf("newuoa_difftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
