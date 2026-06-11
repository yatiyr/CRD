// cobyla_difftest.cpp — Phase 3.1.6 v7-p-2 DIFFERENTIAL-TEST HARNESS (the L-BFGS-B playbook: manual reading is
// NOT verification; the compiled NLopt reference adjudicates the port). Two layers:
//   1. PER-ROUTINE `trstlp` — identical inputs to the oracle (cobyla_exposed.o, statics externalized by
//      scripts/setup-nlopt-ref.sh) and the Cerid port; outputs compared BIT-EXACT: dx (reals), iact (ints),
//      ifull, and the return code — across bug-hiding regimes (feasible-at-zero straight-to-stage-2, violated
//      sets, DUPLICATE/parallel gradients (the L130 linear-dependence path), m = 0, tiny ρ, n = 1).
//   2. END-TO-END `cobyla()` vs `minimize_cobyla` through the `crd_cobyla_e2e` shim (same layer, no NLopt
//      rescaling): same minimizer, same f, SAME EVALUATION COUNT — bit-comparable because the port keeps the
//      reference's exact arithmetic (LCG, float-literal artifacts, goto flow).
// Dev-only, WSL, never CI/shipped. Build (after scripts/setup-nlopt-ref.sh; from the repo root):
//   g++ -std=c++20 -O2 runtime/examples/cobyla_difftest.cpp \
//       build/linux-gcc-release/engine/*/libcrd-{memory,vm,log,core}.a-style libs as available \
//       ~/cerid-deps/nlopt-ref-build/cobyla_exposed.o ~/cerid-deps/nlopt-ref-build/libnlopt_nocobyla.a \
//       -Iengine/core/include -Iengine/containers/include -Iengine/memory/include -Iengine/vm/include \
//       -Iengine/log/include -Iengine/hesap/include -Iengine/hesap-opt/include -Iengine/hesap-sparse/include \
//       -Iengine/hesap-dense/include -Ibuild/linux-gcc-release/engine/core/include -lm -o /tmp/cobyla_difftest

#include <crd/hesap/opt/cobyla.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace opt = crd::hesap::opt;
namespace ci = crd::hesap::opt::detail::cobyla_impl;

extern "C"
{
    // The oracle's exposed trstlp (nlopt_result returned as int; C ABI-compatible).
    int trstlp(int* n, int* m, double* a, double* b, double* rho, double* dx, int* ifull, int* iact, double* z,
               double* zdota, double* vmultc, double* sdirn, double* dxnew, double* vmultd);
    typedef int (*crd_calcfc_plain)(int n, int m, const double* x, double* f, double* con);
    int crd_cobyla_e2e(int n, int m, double* x, double* minf, double rhobeg, double rhoend, int maxeval,
                       double ftol_rel, double ftol_abs, const double* lb, const double* ub, crd_calcfc_plain cb,
                       int* nevals_out);
}

namespace
{

std::mt19937_64 g_rng(0xC0BU);
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

// ------------------------------------------------------------------- per-routine trstlp differential test
void diff_trstlp(int n, int m, const double* a0, const double* b0, double rho, int trial)
{
    const int mp1 = m + 1;
    // Oracle buffers.
    double a_o[8 * 24];
    double b_o[24];
    double dx_o[8];
    int iact_o[24];
    double z_o[64];
    double zdota_o[8];
    double vmultc_o[24];
    double sdirn_o[8];
    double dxnew_o[8];
    double vmultd_o[24];
    int ifull_o = 0;
    // Cerid buffers.
    double a_c[8 * 24];
    double b_c[24];
    double dx_c[8];
    int iact_c[24];
    double z_c[64];
    double zdota_c[8];
    double vmultc_c[24];
    double sdirn_c[8];
    double dxnew_c[8];
    double vmultd_c[24];
    int ifull_c = 0;

    std::memcpy(a_o, a0, sizeof(double) * static_cast<size_t>(n) * static_cast<size_t>(mp1));
    std::memcpy(a_c, a0, sizeof(double) * static_cast<size_t>(n) * static_cast<size_t>(mp1));
    std::memcpy(b_o, b0, sizeof(double) * static_cast<size_t>(mp1));
    std::memcpy(b_c, b0, sizeof(double) * static_cast<size_t>(mp1));

    double rho_o = rho;
    const int rc_o =
        trstlp(&n, &m, a_o, b_o, &rho_o, dx_o, &ifull_o, iact_o, z_o, zdota_o, vmultc_o, sdirn_o, dxnew_o, vmultd_o);
    const double rho_c = rho;
    const ci::Rc rc_c = ci::trstlp<double>(&n, &m, a_c, b_c, &rho_c, dx_c, &ifull_c, iact_c, z_c, zdota_c, vmultc_c,
                                           sdirn_c, dxnew_c, vmultd_c);

    check(static_cast<int>(rc_c) == rc_o, "trstlp return code", trial);
    check(ifull_c == ifull_o, "trstlp ifull", trial);
    check(std::memcmp(dx_c, dx_o, sizeof(double) * static_cast<size_t>(n)) == 0, "trstlp dx bit-exact", trial);
    check(std::memcmp(iact_c, iact_o, sizeof(int) * static_cast<size_t>(mp1)) == 0, "trstlp iact", trial);
    check(std::memcmp(vmultc_c, vmultc_o, sizeof(double) * static_cast<size_t>(mp1)) == 0, "trstlp vmultc bit-exact",
          trial);
}

// ------------------------------------------------------------------------------- end-to-end shared problems
struct E2eProblem
{
    const char* name;
    int n;
    int m;
    crd_calcfc_plain cb;
    const double* x0;
    const double* lb;
    const double* ub;
    double rhobeg;
    double rhoend;
};

int cb_disk_product(int, int, const double* x, double* f, double* con)
{
    *f = x[0] * x[1];
    con[0] = 1.0 - x[0] * x[0] - x[1] * x[1];
    return 0;
}

int cb_rosen_disk(int, int, const double* x, double* f, double* con)
{
    const double a = 1.0 - x[0];
    const double b = x[1] - x[0] * x[0];
    *f = a * a + 100.0 * b * b;
    con[0] = 1.0 - x[0] * x[0] - x[1] * x[1];
    return 0;
}

int cb_rosen(int, int, const double* x, double* f, double*)
{
    const double a = 1.0 - x[0];
    const double b = x[1] - x[0] * x[0];
    *f = a * a + 100.0 * b * b;
    return 0;
}

int cb_sum(int, int, const double* x, double* f, double*)
{
    *f = x[0] + x[1];
    return 0;
}

// Cerid-side adapters over the same callbacks.
class CbObjective final : public opt::Objective<double>
{
public:
    CbObjective(crd_calcfc_plain cb, int n, int m) noexcept : Objective<double>(false, false), m_cb(cb), m_n(n), m_m(m)
    {
    }
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        double f = 0.0;
        double con[8];
        (void)m_cb(m_n, m_m, x.data(), &f, con);
        return f;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return static_cast<crd::usize>(m_n); }

private:
    crd_calcfc_plain m_cb;
    int m_n;
    int m_m;
};

class CbConstraints final : public opt::Constraints<double>
{
public:
    CbConstraints(crd_calcfc_plain cb, int n, int m) noexcept : Constraints<double>(false), m_cb(cb), m_n(n), m_m(m) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return static_cast<crd::usize>(m_m); }
    [[nodiscard]] crd::usize n() const noexcept override { return static_cast<crd::usize>(m_n); }
    void eval(crd::containers::ConstSpan<double> x, crd::containers::Span<double>,
              crd::containers::Span<double> ci_out) const override
    {
        double f = 0.0;
        (void)m_cb(m_n, m_m, x.data(), &f, ci_out.data());
    }

private:
    crd_calcfc_plain m_cb;
    int m_n;
    int m_m;
};

void diff_e2e(const E2eProblem& p, crd::memory::IAllocator* alloc)
{
    const double huge = HUGE_VAL;
    double lb[8];
    double ub[8];
    for (int i = 0; i < p.n; ++i)
    {
        lb[i] = p.lb != nullptr ? p.lb[i] : -huge;
        ub[i] = p.ub != nullptr ? p.ub[i] : huge;
    }
    // Oracle.
    double x_o[8];
    std::memcpy(x_o, p.x0, sizeof(double) * static_cast<size_t>(p.n));
    double minf_o = 0.0;
    int nevals_o = 0;
    const int rc_o =
        crd_cobyla_e2e(p.n, p.m, x_o, &minf_o, p.rhobeg, p.rhoend, 100000, 0.0, 0.0, lb, ub, p.cb, &nevals_o);
    // Cerid.
    const CbObjective obj(p.cb, p.n, p.m);
    const CbConstraints cons(p.cb, p.n, p.m);
    opt::CobylaOptions<double> co;
    co.rhobeg = p.rhobeg;
    co.rhoend = p.rhoend;
    co.max_evals = 100000;
    const opt::OptResult<double> r =
        opt::minimize_cobyla<double>(obj, p.m > 0 ? &cons : nullptr, {p.x0, static_cast<crd::usize>(p.n)},
                                     {lb, static_cast<crd::usize>(p.n)}, {ub, static_cast<crd::usize>(p.n)}, alloc, co);

    char what[128];
    std::snprintf(what, sizeof(what), "e2e %s rc (oracle %d)", p.name, rc_o);
    check(rc_o >= 1, what, 0); // the oracle terminated normally
    std::snprintf(what, sizeof(what), "e2e %s nevals (oracle %d, cerid %u)", p.name, nevals_o,
                  static_cast<unsigned>(r.fn_evals));
    check(static_cast<int>(r.fn_evals) == nevals_o, what, 0);
    std::snprintf(what, sizeof(what), "e2e %s minf bit-exact (oracle %.17g, cerid %.17g)", p.name, minf_o, r.fx);
    check(std::memcmp(&minf_o, &r.fx, sizeof(double)) == 0, what, 0);
    for (int i = 0; i < p.n; ++i)
    {
        std::snprintf(what, sizeof(what), "e2e %s x[%d] bit-exact (oracle %.17g, cerid %.17g)", p.name, i, x_o[i],
                      r.x[static_cast<crd::usize>(i)]);
        check(std::memcmp(&x_o[i], &r.x[static_cast<crd::usize>(i)], sizeof(double)) == 0, what, 0);
    }
}

} // namespace

int main()
{
    // ---------------------------------------------------------------- per-routine trstlp (targeted + random)
    // Targeted regimes.
    {
        // Feasible at zero (b <= 0): straight to stage 2.
        const double a[] = {1.0, 0.5, -0.3, 0.2, 0.7, -1.1}; // n=2, m=2 -> mp1=3 columns
        const double b[] = {-0.5, -0.1, 0.4};
        diff_trstlp(2, 2, a, b, 1.0, -1);
    }
    {
        // Violated set (stage 1 work).
        const double a[] = {1.0, 0.0, 0.0, 1.0, 1.0, 1.0};
        const double b[] = {0.5, 0.3, 0.2};
        diff_trstlp(2, 2, a, b, 0.25, -2);
    }
    {
        // DUPLICATE gradients (the L130 linear-dependence path).
        const double a[] = {1.0, 1.0, 1.0, 1.0, 0.5, -0.5};
        const double b[] = {0.4, 0.6, 0.1};
        diff_trstlp(2, 2, a, b, 0.5, -3);
    }
    {
        // m = 0 (objective only).
        const double a[] = {0.7, -0.3, 0.4};
        const double b[] = {0.0};
        diff_trstlp(3, 0, a, b, 1.0, -4);
    }
    {
        // n = 1.
        const double a[] = {1.0, -2.0};
        const double b[] = {0.3, 0.1};
        diff_trstlp(1, 1, a, b, 0.05, -5);
    }
    // Randomized sweep.
    for (int trial = 0; trial < 400; ++trial)
    {
        const int n = 1 + static_cast<int>(g_rng() % 6);
        const int m = static_cast<int>(g_rng() % 9);
        double a[8 * 24];
        double b[24];
        for (int k = 0; k < m + 1; ++k)
        {
            for (int i = 0; i < n; ++i)
            {
                a[k * n + i] = rr(-2.0, 2.0);
            }
            b[k] = rr(-1.0, 1.0);
        }
        if (m >= 2 && trial % 5 == 0) // inject a duplicate row regularly
        {
            for (int i = 0; i < n; ++i)
            {
                a[1 * n + i] = a[0 * n + i];
            }
        }
        const double rho = trial % 3 == 0 ? 1e-3 : (trial % 3 == 1 ? 0.5 : 5.0);
        diff_trstlp(n, m, a, b, rho, trial);
    }

    // ----------------------------------------------------------------------------- end-to-end shared problems
    crd::memory::TlsfAllocator alloc(1U << 24);
    const double x0_disk[] = {1.0, 1.0};
    const double x0_zero[] = {0.0, 0.0};
    const double x0_rosen[] = {-1.2, 1.0};
    const double x0_sum[] = {3.0, 4.0};
    const double lo_box[] = {-2.0, -2.0};
    const double up_box[] = {2.0, 2.0};
    const double lo_sum[] = {1.0, 2.0};
    const double up_sum[] = {10.0, 10.0};
    const E2eProblem problems[] = {
        {"disk-product", 2, 1, cb_disk_product, x0_disk, nullptr, nullptr, 0.5, 1e-10},
        {"rosen-in-disk", 2, 1, cb_rosen_disk, x0_zero, nullptr, nullptr, 0.5, 1e-10},
        {"rosen-unconstrained", 2, 0, cb_rosen, x0_rosen, nullptr, nullptr, 1.0, 1e-8},
        {"rosen-boxed", 2, 0, cb_rosen, x0_rosen, lo_box, up_box, 1.0, 1e-10},
        {"sum-bounds-active", 2, 0, cb_sum, x0_sum, lo_sum, up_sum, 0.5, 1e-10},
    };
    for (const E2eProblem& p : problems)
    {
        diff_e2e(p, &alloc);
    }

    std::printf("cobyla_difftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
