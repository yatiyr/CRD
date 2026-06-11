// opt_scoreboard.cpp — v7-z: the gold-standard scoreboard, CERID side. The SAME formula-pinned problems as
// scripts/opt_scoreboard.py (the reference side); prints objective / eval-count / wall-clock rows for the
// honest comparison table. Dev-only; build standalone on WSL with the difftest recipe (g++ + the
// linux-gcc-release crd libs), run next to the Python script on the same machine.

#include <crd/hesap/opt/cmaes.hpp>
#include <crd/hesap/opt/conic.hpp>
#include <crd/hesap/opt/global_search.hpp>
#include <crd/hesap/opt/lp.hpp>
#include <crd/hesap/opt/mip.hpp>
#include <crd/hesap/opt/nelder_mead.hpp>
#include <crd/hesap/opt/nlp_interior_point.hpp>
#include <crd/hesap/opt/powell.hpp>
#include <crd/hesap/opt/qp.hpp>
#include <crd/hesap/opt/qp_active_set.hpp>
#include <crd/hesap/opt/stochastic.hpp>
#include <crd/hesap/opt/trust_region.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace opt = crd::hesap::opt;

namespace
{
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kPi2 = 6.283185307179586476925286766559;

template <typename F> double time_ms(F&& fn, int repeats = 3)
{
    double best = kInf;
    for (int r = 0; r < repeats; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = ms < best ? ms : best;
    }
    return best;
}

// ---------------------------------------------------------------- the formula-pinned QP (n=30, m=40)
void row_qp(crd::memory::IAllocator* alloc)
{
    constexpr crd::usize n = 30;
    constexpr crd::usize m = 40;
    static double p[n * n];
    static double q[n];
    static double a[m * n];
    static double l[m];
    static double u[m];
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            p[i * n + j] = i == j ? 10.0 : 1.0 / (1.0 + std::fabs(static_cast<double>(i) - static_cast<double>(j)));
        }
        q[i] = std::sin(static_cast<double>(i) + 1.0);
    }
    for (crd::usize k = 0; k < m; ++k)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a[k * n + j] = std::cos((static_cast<double>(k) + 1.0) * (static_cast<double>(j) + 1.0) / 10.0);
        }
        l[k] = -1.0 - static_cast<double>(k % 3);
        u[k] = 1.0 + static_cast<double>(k % 5) * 0.5;
    }
    const opt::QpProblem<double> prob{{p, n * n}, {q, n}, {a, m * n}, {l, m}, {u, m}, n, m};

    opt::QpAdmmOptions<double> ao;
    ao.eps_abs = 1e-8;
    ao.eps_rel = 1e-8;
    opt::QpResult<double> ra{alloc};
    const double ms_a = time_ms([&] { ra = opt::solve_qp_admm<double>(prob, ao, alloc); });
    std::printf("QP  cerid ADMM    : obj %+.9f  iters %5u  %8.2f ms\n", ra.obj, static_cast<unsigned>(ra.iterations),
                ms_a);
    opt::QpResult<double> rm{alloc};
    const double ms_m = time_ms([&] { rm = opt::solve_qp_mehrotra<double>(prob, {}, alloc); });
    std::printf("QP  cerid Mehrotra: obj %+.9f  iters %5u  %8.2f ms\n", rm.obj, static_cast<unsigned>(rm.iterations),
                ms_m);
    opt::QpResult<double> rg{alloc};
    const double ms_g = time_ms([&] { rg = opt::solve_qp_goldfarb_idnani<double>(prob, alloc); });
    std::printf("QP  cerid GI      : obj %+.9f  (dual active-set)   %8.2f ms\n", rg.obj, ms_g);
}

// ---------------------------------------------------------------- the formula-pinned LP (n=40, m=25)
void row_lp(crd::memory::IAllocator* alloc)
{
    constexpr crd::usize n = 40;
    constexpr crd::usize m = 25;
    static double c[n];
    static double a[m * n];
    static double l[m];
    static double u[m];
    static double xlo[n];
    static double xup[n];
    for (crd::usize j = 0; j < n; ++j)
    {
        c[j] = std::cos(static_cast<double>(j) + 1.0);
        xlo[j] = -1.0;
        xup[j] = 2.0;
    }
    for (crd::usize k = 0; k < m; ++k)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            a[k * n + j] = std::sin((static_cast<double>(k) + 2.0) * (static_cast<double>(j) + 1.0) / 7.0) + 1.1;
        }
        l[k] = -kInf;
        u[k] = 5.0 + static_cast<double>(k % 7);
    }
    const opt::LpProblem<double> prob{{c, n}, {a, m * n}, {l, m}, {u, m}, {xlo, n}, {xup, n}, n, m};
    opt::LpResult<double> rs{alloc};
    const double ms_s = time_ms([&] { rs = opt::solve_lp_simplex<double>(prob, alloc); });
    std::printf("LP  cerid simplex : obj %+.9f  pivots %4u    %8.2f ms\n", rs.obj, static_cast<unsigned>(rs.iterations),
                ms_s);
    opt::LpResult<double> ri{alloc};
    const double ms_i = time_ms([&] { ri = opt::solve_lp_mehrotra<double>(prob, alloc); });
    std::printf("LP  cerid IPM     : obj %+.9f  iters %5u    %8.2f ms\n", ri.obj, static_cast<unsigned>(ri.iterations),
                ms_i);
}

// ---------------------------------------------------------------- the SOCP norm ball
void row_socp(crd::memory::IAllocator* alloc)
{
    const double c[] = {1.0, 1.0};
    const double a[] = {0.0, 0.0, -1.0, 0.0, 0.0, -1.0};
    const double b[] = {0.5, -1.0, -2.0};
    const opt::ConeDesc cones[] = {{opt::ConeType::Soc, 3}};
    const opt::ConicProblem<double> prob{{c, 2}, {a, 6}, {b, 3}, {cones, 1}, 2, 3};
    opt::ConicAdmmOptions<double> co;
    co.eps_abs = 1e-9;
    co.eps_rel = 1e-9;
    opt::ConicResult<double> r{alloc};
    const double ms = time_ms([&] { r = opt::solve_conic_admm<double>(prob, alloc, co); });
    std::printf("SOCP cerid ADMM   : obj %+.9f  iters %5u  %8.2f ms   (analytic %+.9f)\n", r.obj,
                static_cast<unsigned>(r.iterations), ms, 3.0 - 0.5 * std::sqrt(2.0));
}

// ---------------------------------------------------------------- MIP knapsack (n=12, formula data)
void row_mip(crd::memory::IAllocator* alloc)
{
    constexpr crd::usize n = 12;
    static double c[n];
    static double w[n];
    double wsum = 0.0;
    for (crd::usize j = 0; j < n; ++j)
    {
        c[j] = -(5.0 + static_cast<double>((j * 7) % 13)); // min -v.x
        w[j] = 3.0 + static_cast<double>((j * 5) % 11);
        wsum += w[j];
    }
    const double wcap = wsum / 3.0;
    static double xlo[n];
    static double xup[n];
    static bool mask[n];
    for (crd::usize j = 0; j < n; ++j)
    {
        xlo[j] = 0.0;
        xup[j] = 1.0;
        mask[j] = true;
    }
    const double l[] = {-kInf};
    const double u[] = {wcap};
    const opt::LpProblem<double> prob{{c, n}, {w, n}, {l, 1}, {u, 1}, {xlo, n}, {xup, n}, n, 1};
    opt::MipResult<double> r{alloc};
    const double ms = time_ms([&] { r = opt::solve_mip_branch_and_bound<double>(prob, {mask, n}, alloc); });
    std::printf("MIP cerid B&B     : obj %+.9f  nodes %5u   %8.2f ms\n", r.obj, static_cast<unsigned>(r.nodes), ms);
}

// ---------------------------------------------------------------- nonlinear rows
class Rosen2 final : public opt::Objective<double>
{
public:
    Rosen2() noexcept : Objective<double>(false, false) {}
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        const double a = 1.0 - x[0];
        const double b = x[1] - x[0] * x[0];
        return a * a + 100.0 * b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

class Rosen2Full final : public opt::Objective<double> // + gradient + Hessian for the trust-region rows
{
public:
    Rosen2Full() noexcept : Objective<double>(true, true, /*has_hessian=*/true) {}
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        const double a = 1.0 - x[0];
        const double b = x[1] - x[0] * x[0];
        return a * a + 100.0 * b * b;
    }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<double> x, crd::containers::Span<double> g) const override
    {
        g[0] = -2.0 * (1.0 - x[0]) - 400.0 * x[0] * (x[1] - x[0] * x[0]);
        g[1] = 200.0 * (x[1] - x[0] * x[0]);
        return true;
    }
    [[nodiscard]] bool hessian_vector(crd::containers::ConstSpan<double> x, crd::containers::ConstSpan<double> v,
                                      crd::containers::Span<double> hv) const override
    {
        const double h00 = 2.0 - 400.0 * (x[1] - 3.0 * x[0] * x[0]);
        const double h01 = -400.0 * x[0];
        hv[0] = h00 * v[0] + h01 * v[1];
        hv[1] = h01 * v[0] + 200.0 * v[1];
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<double> x, crd::containers::Span<double> h) const override
    {
        h[0] = 2.0 - 400.0 * (x[1] - 3.0 * x[0] * x[0]);
        h[1] = -400.0 * x[0];
        h[2] = -400.0 * x[0];
        h[3] = 200.0;
        return true;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
};

class SphereN final : public opt::Objective<double>
{
public:
    explicit SphereN(crd::usize n) noexcept : Objective<double>(false, false), m_n(n) {}
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        double acc = 0.0;
        for (crd::usize i = 0; i < m_n; ++i)
        {
            acc += x[i] * x[i];
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

private:
    crd::usize m_n;
};

class RosenN final : public opt::Objective<double>
{
public:
    explicit RosenN(crd::usize n) noexcept : Objective<double>(false, false), m_n(n) {}
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        double acc = 0.0;
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const double a = 1.0 - x[i];
            const double b = x[i + 1] - x[i] * x[i];
            acc += a * a + 100.0 * b * b;
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

private:
    crd::usize m_n;
};

class Rastrigin4 final : public opt::Objective<double>
{
public:
    Rastrigin4() noexcept : Objective<double>(false, false) {}
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        double acc = 40.0;
        for (crd::usize i = 0; i < 4; ++i)
        {
            acc += x[i] * x[i] - 10.0 * std::cos(kPi2 * x[i]);
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 4; }
};

void row_nonlinear(crd::memory::IAllocator* alloc)
{
    const Rosen2 obj;
    const double x0[] = {-1.2, 1.0};
    opt::OptOptions<double> opts;
    opts.max_iters = 5000;

    opt::NelderMeadOptions<double> nm; // matched to the scipy run (xatol/fatol 1e-8)
    nm.max_fun = 20000;
    opt::OptResult<double> rn{alloc};
    const double ms_n = time_ms([&] { rn = opt::minimize_nelder_mead<double>(obj, {x0, 2}, opts, alloc, nm); });
    std::printf("DFO cerid NM      : f %.3e  nfev %5u      %8.2f ms\n", rn.fx, static_cast<unsigned>(rn.fn_evals),
                ms_n);
    opt::OptResult<double> rp{alloc};
    opt::PowellOptions<double> po; // scipy's exact drive (xtol=ftol=1e-4; the inner Brent runs at 100·xtol)
    po.xtol = 1e-4;
    po.ftol = 1e-4;
    const double ms_p = time_ms([&] { rp = opt::minimize_powell<double>(obj, {x0, 2}, opts, alloc, po); });
    std::printf("DFO cerid Powell  : f %.3e  nfev %5u      %8.2f ms\n", rp.fx, static_cast<unsigned>(rp.fn_evals),
                ms_p);

    const Rosen2Full objf;
    opt::OptOptions<double> topts;
    topts.grad_tol = 1e-9;
    topts.max_iters = 500;
    const opt::TrustRegionSubproblem kinds[] = {opt::TrustRegionSubproblem::SteihaugCg,
                                                opt::TrustRegionSubproblem::TrustKrylov,
                                                opt::TrustRegionSubproblem::Exact};
    const char* names[] = {"steihaug ", "gltr     ", "exact    "};
    for (int k = 0; k < 3; ++k)
    {
        opt::OptResult<double> rt{alloc};
        const double ms = time_ms(
            [&]
            {
                opt::TrustRegionOptions<double> tro;
                tro.subproblem = kinds[k];
                rt = opt::minimize_trust_region<double>(objf, {x0, 2}, topts, alloc, tro);
            });
        std::printf("TR  cerid %s: f %.3e  nfev %4u njev %4u  %8.2f ms\n", names[k], rt.fx,
                    static_cast<unsigned>(rt.fn_evals), static_cast<unsigned>(rt.grad_evals), ms);
    }

    const Rastrigin4 robj;
    const double lo[] = {-5.12, -5.12, -5.12, -5.12};
    const double up[] = {5.12, 5.12, 5.12, 5.12};
    opt::DeOptions<double> de;
    de.tol = 1e-8;
    de.max_gens = 2000;
    opt::OptResult<double> rd{alloc};
    const double ms_d =
        time_ms([&] { rd = opt::minimize_differential_evolution<double>(robj, {lo, 4}, {up, 4}, alloc, de); },
                /*repeats=*/1);
    std::printf("GLB cerid DE      : f %.3e  nfev %6u     %8.2f ms   (Rastrigin-4 global = 0)\n", rd.fx,
                static_cast<unsigned>(rd.fn_evals), ms_d);

    opt::BasinHoppingOptions<double> bh;
    bh.local_minimizer = opt::BasinHoppingLocal::LbfgsFd; // scipy basinhopping's default local method
    opt::OptResult<double> rb{alloc};
    const double ms_b = time_ms([&] { rb = opt::minimize_basin_hopping<double>(obj, {x0, 2}, alloc, bh); },
                                /*repeats=*/1);
    std::printf("GLB cerid BH      : f %.3e  nfev %6u     %8.2f ms\n", rb.fx, static_cast<unsigned>(rb.fn_evals), ms_b);
}

void row_cmaes(crd::memory::IAllocator* alloc)
{
    const SphereN sph(8);
    static double x0s[8];
    for (crd::usize i = 0; i < 8; ++i)
    {
        x0s[i] = 2.0;
    }
    opt::CmaesOptions<double> co;
    co.sigma0 = 1.0;
    co.ftol = 1e-10; // ~ pycma ftarget 1e-10
    co.seed = 7;
    opt::OptResult<double> rs{alloc};
    const double ms_s = time_ms([&] { rs = opt::minimize_cmaes<double>(sph, {x0s, 8}, alloc, co); }, 1);
    std::printf("GLB cerid CMA sph8: f %.3e  evals %6u    %8.2f ms\n", rs.fx, static_cast<unsigned>(rs.fn_evals), ms_s);

    const RosenN ros(5);
    static double x0r[5];
    for (crd::usize i = 0; i < 5; ++i)
    {
        x0r[i] = -2.0;
    }
    opt::CmaesOptions<double> cr;
    cr.sigma0 = 0.5;
    cr.ftol = 1e-8;
    cr.max_evals = 200000;
    cr.seed = 7;
    opt::OptResult<double> rr{alloc};
    const double ms_r = time_ms([&] { rr = opt::minimize_cmaes<double>(ros, {x0r, 5}, alloc, cr); }, 1);
    std::printf("GLB cerid CMA ros5: f %.3e  evals %6u    %8.2f ms\n", rr.fx, static_cast<unsigned>(rr.fn_evals), ms_r);
}
// The NLP row vs IPOPT: the Wächter-Biegler filter IPM on the pinned Rosenbrock-in-the-unit-disk instance
// (min rosen2 s.t. 1 − x² − y² ≥ 0, from (0,0); scipy reference x* = (0.78642, 0.61770), active boundary).
class DiskCons final : public opt::Constraints<double>
{
public:
    DiskCons() noexcept : Constraints<double>(/*has_jacobians=*/true, /*has_lagrangian_hessian=*/true) {}
    [[nodiscard]] crd::usize num_eq() const noexcept override { return 0; }
    [[nodiscard]] crd::usize num_ineq() const noexcept override { return 1; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    void eval(crd::containers::ConstSpan<double> x, crd::containers::Span<double>,
              crd::containers::Span<double> ci) const override
    {
        ci[0] = 1.0 - x[0] * x[0] - x[1] * x[1];
    }
    [[nodiscard]] bool jacobians(crd::containers::ConstSpan<double> x, crd::containers::Span<double>,
                                 crd::containers::Span<double> ji) const override
    {
        ji[0] = -2.0 * x[0];
        ji[1] = -2.0 * x[1];
        return true;
    }
    [[nodiscard]] bool add_lagrangian_hessian(crd::containers::ConstSpan<double>, crd::containers::ConstSpan<double>,
                                              crd::containers::ConstSpan<double> mu,
                                              crd::containers::Span<double> h) const override
    {
        h[0] -= mu[0] * (-2.0); // −μ·∇²c_I, ∇²c_I = −2I
        h[3] -= mu[0] * (-2.0);
        return true;
    }
};

void row_nlp_ipm(crd::memory::IAllocator* alloc)
{
    const Rosen2Full obj;
    const DiskCons cons;
    const double x0[] = {0.0, 0.0};
    opt::OptOptions<double> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 200;
    opt::OptResult<double> r{alloc};
    const double ms = time_ms([&] { r = opt::minimize_interior_point<double>(obj, cons, {x0, 2}, opts, alloc); });
    std::printf("NLP cerid IPM : f %.9f  x [%.7f, %.7f]  nit %4u   %8.2f ms\n", r.fx, r.x[0], r.x[1],
                static_cast<unsigned>(r.iterations), ms);
}

// Live torch-trajectory parity: the SAME recurrence as torch.optim.{Adam, AdamW} (rosen2 from (-1.2, 1),
// exact gradients, lr=0.05, 200 pinned steps). torch rounds sqrt(v)/sqrt(bc2)+eps where Kingma/Cerid round
// sqrt(v/bc2)+eps — agreement is ~1e-12-class by construction, not bit-exact.
void row_torch_parity(crd::memory::IAllocator* alloc)
{
    const Rosen2Full obj;
    for (int dec = 0; dec < 2; ++dec)
    {
        double x[] = {-1.2, 1.0};
        const double ms = time_ms(
            [&]
            {
                x[0] = -1.2;
                x[1] = 1.0;
                opt::AdamConfig<double> cfg;
                cfg.lr = 0.05;
                cfg.decoupled = dec == 1;
                cfg.weight_decay = dec == 1 ? 0.01 : 0.0;
                opt::AdamOptimizer<double> adam(alloc, 2, cfg);
                double g[2];
                for (int t = 0; t < 200; ++t)
                {
                    (void)obj.gradient({x, 2}, {g, 2});
                    adam.step({x, 2}, {g, 2});
                }
            },
            /*repeats=*/1);
        const double f = obj.value({x, 2});
        std::printf("OPT cerid %s: f %.12e  x [%.12f, %.12f]   %8.2f ms (200 steps lr=0.05)\n",
                    dec == 1 ? "AdamW" : "Adam ", f, x[0], x[1], ms);
    }
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    std::printf("== v7-z gold-standard scoreboard — CERID side ==\n");
    row_qp(&alloc);
    row_lp(&alloc);
    row_socp(&alloc);
    row_mip(&alloc);
    row_nonlinear(&alloc);
    row_cmaes(&alloc);
    row_torch_parity(&alloc);
    row_nlp_ipm(&alloc);
    return 0;
}
