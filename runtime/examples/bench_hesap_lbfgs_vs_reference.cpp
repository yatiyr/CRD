// bench_hesap_lbfgs_vs_reference.cpp — Phase 3.1.6 v7-d gold-standard scoreboard: Cerid L-BFGS vs liblbfgs
// (Naoaki Okazaki's reference C L-BFGS, whose DEFAULT line search IS More-Thuente). Dev-only, gated behind
// CRD_BUILD_HESAP_VS_LBFGS; never shipped/CI.
//
// HONESTY (advisor + the v6-z lesson): L-BFGS wall-clock is ~entirely iterations × (fn+grad) evaluations — a
// wall-clock "win" is manufacturable by a looser tolerance / different m / cheaper line search. The DISCRIMINATING
// verdict is ITERATION COUNT and POINT-EVALUATION COUNT at MATCHED m + matched line search + matched accuracy. We
// therefore (1) feed BOTH solvers the SAME analytic f+g (no forward-AD — that would make Cerid's wall-clock
// meaningless), (2) match m=8, More-Thuente, c1=1e-4, c2=0.9, (3) report the ACHIEVED ‖g‖∞ for each so matched
// accuracy is visible. POINT counts compare apples-to-apples (Cerid fn_evals == liblbfgs evaluate calls); Cerid's
// interface calls value() then gradient() separately (2 passes/point) vs liblbfgs's fused 1 pass — the reserved
// value_and_gradient slot's opportunity.
//
// CORPUS (advisor: one function family is thin for a "no eval gap" claim): Rosenbrock-N (curved valley) + Powell
// singular (singular Hessian at the solution) + Beale (low-dim, distinct curvature) — three More/Garbow/Hillstrom
// classes, so eval-parity is earned across function shapes, not asserted from one.

#include <crd/hesap/opt/opt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <lbfgs.h>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace opt = crd::hesap::opt;

namespace
{
enum class TestFn
{
    Rosenbrock, // Σ (1−x_i)² + 100(x_{i+1}−x_i²)²        ; x*=ones,  f*=0
    Powell,     // Powell singular (n multiple of 4)       ; x*=0,     f*=0 (singular Hessian at x*)
    Beale,      // (1.5−x+xy)²+(2.25−x+xy²)²+(2.625−x+xy³)²; x*=(3,.5),f*=0  (n=2)
};

// Shared analytic f + g (the SAME formula both solvers see). Returns f, fills g (length n).
double eval_fg(TestFn fn, const double* x, crd::usize n, double* g)
{
    for (crd::usize i = 0; i < n; ++i)
    {
        g[i] = 0.0;
    }
    double f = 0.0;
    if (fn == TestFn::Rosenbrock)
    {
        for (crd::usize i = 0; i + 1 < n; ++i)
        {
            const double a = 1.0 - x[i];
            const double b = x[i + 1] - x[i] * x[i];
            f += a * a + 100.0 * b * b;
            g[i] += -2.0 * a - 400.0 * x[i] * b;
            g[i + 1] += 200.0 * b;
        }
    }
    else if (fn == TestFn::Powell)
    {
        for (crd::usize j = 0; j + 3 < n; j += 4)
        {
            const double t1 = x[j] + 10.0 * x[j + 1];
            const double t2 = x[j + 2] - x[j + 3];
            const double t3 = x[j + 1] - 2.0 * x[j + 2];
            const double t4 = x[j] - x[j + 3];
            f += t1 * t1 + 5.0 * t2 * t2 + t3 * t3 * t3 * t3 + 10.0 * t4 * t4 * t4 * t4;
            g[j] += 2.0 * t1 + 40.0 * t4 * t4 * t4;
            g[j + 1] += 20.0 * t1 + 4.0 * t3 * t3 * t3;
            g[j + 2] += 10.0 * t2 - 8.0 * t3 * t3 * t3;
            g[j + 3] += -10.0 * t2 - 40.0 * t4 * t4 * t4;
        }
    }
    else // Beale (n == 2)
    {
        const double f1 = 1.5 - x[0] + x[0] * x[1];
        const double f2 = 2.25 - x[0] + x[0] * x[1] * x[1];
        const double f3 = 2.625 - x[0] + x[0] * x[1] * x[1] * x[1];
        f = f1 * f1 + f2 * f2 + f3 * f3;
        g[0] = 2.0 * f1 * (x[1] - 1.0) + 2.0 * f2 * (x[1] * x[1] - 1.0) + 2.0 * f3 * (x[1] * x[1] * x[1] - 1.0);
        g[1] = 2.0 * f1 * x[0] + 2.0 * f2 * (2.0 * x[0] * x[1]) + 2.0 * f3 * (3.0 * x[0] * x[1] * x[1]);
    }
    return f;
}

void init_x0(TestFn fn, double* x, crd::usize n)
{
    if (fn == TestFn::Rosenbrock)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = (i % 2 == 0) ? -1.2 : 1.0;
        }
    }
    else if (fn == TestFn::Powell)
    {
        for (crd::usize j = 0; j + 3 < n; j += 4)
        {
            x[j] = 3.0;
            x[j + 1] = -1.0;
            x[j + 2] = 0.0;
            x[j + 3] = 1.0;
        }
    }
    else // Beale
    {
        x[0] = 1.0;
        x[1] = 1.0;
    }
}

const char* fn_name(TestFn fn)
{
    switch (fn)
    {
    case TestFn::Rosenbrock: return "Rosenbrock";
    case TestFn::Powell:     return "Powell-sing";
    case TestFn::Beale:      return "Beale";
    }
    return "?";
}

class TestObjective final : public opt::Objective<double>
{
public:
    TestObjective(TestFn fn, crd::usize n)
        : opt::Objective<double>(/*has_gradient=*/true, /*has_hessian_vector=*/false), m_fn(fn), m_n(n),
          m_scratch(nullptr)
    {
    }
    void set_scratch(double* s) { m_scratch = s; }
    [[nodiscard]] double value(crd::containers::ConstSpan<double> x) const override
    {
        return eval_fg(m_fn, x.data(), m_n, m_scratch);
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<double> x, crd::containers::Span<double> g) const override
    {
        (void)eval_fg(m_fn, x.data(), m_n, g.data());
        return true;
    }

private:
    TestFn     m_fn;
    crd::usize m_n;
    double*    m_scratch;
};

struct LbfgsInstance
{
    TestFn     fn;
    crd::usize n;
    crd::usize evals = 0;
    crd::usize iters = 0;
};

lbfgsfloatval_t lbfgs_evaluate(void* instance, const lbfgsfloatval_t* x, lbfgsfloatval_t* g, const int /*n*/,
                               const lbfgsfloatval_t /*step*/)
{
    auto* inst = static_cast<LbfgsInstance*>(instance);
    ++inst->evals;
    return eval_fg(inst->fn, x, inst->n, g);
}

int lbfgs_progress(void* instance, const lbfgsfloatval_t*, const lbfgsfloatval_t*, const lbfgsfloatval_t,
                   const lbfgsfloatval_t, const lbfgsfloatval_t, const lbfgsfloatval_t, int, int, int)
{
    ++static_cast<LbfgsInstance*>(instance)->iters;
    return 0;
}

double inf_norm(const double* v, crd::usize n)
{
    double m = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        m = std::fabs(v[i]) > m ? std::fabs(v[i]) : m;
    }
    return m;
}

double now_ms()
{
    using namespace std::chrono;
    return static_cast<double>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) / 1e6;
}

struct Problem
{
    TestFn     fn;
    crd::usize n;
};
} // namespace

int main()
{
    constexpr crd::usize kMemory = 8;
    constexpr int        kReps = 5;
    const Problem        problems[] = {{TestFn::Rosenbrock, 2},   {TestFn::Rosenbrock, 10},
                                       {TestFn::Rosenbrock, 100}, {TestFn::Rosenbrock, 1000},
                                       {TestFn::Powell, 4},       {TestFn::Powell, 40},
                                       {TestFn::Beale, 2}};

    std::printf("# Cerid L-BFGS vs liblbfgs (m=%zu, More-Thuente, c1=1e-4 c2=0.9, best of %d)\n", kMemory, kReps);
    std::printf("# %-16s | %-32s | %-32s | %-10s\n", "problem (N)", "Cerid  (it / evals / |g|inf / ms)",
                "liblbfgs (it / evals / |g|inf / ms)", "eval ratio");
    std::printf("# %s\n", "------------------------------------------------------------------------------------------"
                          "--------------------------");

    crd::memory::TlsfAllocator alloc(1U << 26);

    for (const Problem& prob : problems)
    {
        const crd::usize nn = prob.n;
        crd::containers::Array<double> x0(&alloc);
        crd::containers::Array<double> scratch(&alloc);
        x0.resize(nn);
        scratch.resize(nn);
        init_x0(prob.fn, x0.data(), nn);

        // ---- Cerid ----
        opt::OptOptions<double> opts;
        opts.grad_tol = 1e-6;
        opts.max_iters = 200000;
        TestObjective cobj(prob.fn, nn);
        cobj.set_scratch(scratch.data());
        opt::OptResult<double> cr(&alloc);
        double cerid_best = 1e300;
        for (int rep = 0; rep < kReps; ++rep)
        {
            const double t0 = now_ms();
            cr = opt::minimize_lbfgs<double>(cobj, {x0.data(), nn}, opts, &alloc, nullptr, kMemory);
            const double dt = now_ms() - t0;
            cerid_best = dt < cerid_best ? dt : cerid_best;
        }

        // ---- liblbfgs ----
        lbfgs_parameter_t param;
        lbfgs_parameter_init(&param);
        param.m = static_cast<int>(kMemory);
        param.epsilon = 1e-6;
        param.linesearch = LBFGS_LINESEARCH_MORETHUENTE;
        param.ftol = 1e-4;
        param.gtol = 0.9;
        param.max_iterations = 200000;

        LbfgsInstance lib_final{prob.fn, nn};
        double        lib_best = 1e300;
        double        lib_gnorm = 0.0;
        for (int rep = 0; rep < kReps; ++rep)
        {
            lbfgsfloatval_t* x = lbfgs_malloc(static_cast<int>(nn));
            for (crd::usize i = 0; i < nn; ++i)
            {
                x[i] = x0[i];
            }
            LbfgsInstance   inst{prob.fn, nn};
            lbfgsfloatval_t fx = 0.0;
            const double    t0 = now_ms();
            (void)lbfgs(static_cast<int>(nn), x, &fx, lbfgs_evaluate, lbfgs_progress, &inst, &param);
            const double dt = now_ms() - t0;
            if (dt < lib_best)
            {
                lib_best = dt;
                lib_final = inst;
                crd::containers::Array<double> gtmp(&alloc);
                gtmp.resize(nn);
                (void)eval_fg(prob.fn, x, nn, gtmp.data());
                lib_gnorm = inf_norm(gtmp.data(), nn);
            }
            lbfgs_free(x);
        }

        const double eval_ratio =
            lib_final.evals > 0 ? static_cast<double>(cr.fn_evals) / static_cast<double>(lib_final.evals) : 0.0;
        char label[32];
        std::snprintf(label, sizeof(label), "%s(%zu)", fn_name(prob.fn), nn);
        std::printf("  %-16s | %4zu / %6zu / %.2e / %7.3f | %4zu / %6zu / %.2e / %7.3f | %.2fx\n", label,
                    cr.iterations, cr.fn_evals, cr.grad_norm, cerid_best, lib_final.iters, lib_final.evals,
                    lib_gnorm, lib_best, eval_ratio);
    }

    std::printf("# eval ratio = Cerid point-evals / liblbfgs point-evals at matched accuracy (≈1 ⇒ algorithmically "
                "faithful). Both ‖g‖∞ shown so matched accuracy is visible.\n");
    return 0;
}
