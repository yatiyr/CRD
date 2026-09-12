// crd-hesap-opt v7-d-3 — L-BFGS-B (bound-constrained). The full Zhu-Byrd-Lu-Nocedal port is bit-verified vs the
// reference C by runtime/examples/lbfgsb_difftest.cpp (55 differential checks, incl. end-to-end vs setulb); these
// Catch2 tests cover the public driver: (1) bounded Rosenbrock — the unconstrained minimizer (all ones) is
// INFEASIBLE, so the solver must converge to the constrained minimizer with ≥1 variable pinned to a bound;
// (2) an UNBOUNDED problem reduces to unconstrained L-BFGS (converges to all ones); (3) the active-bound
// determinism moat {1,2,4,8,16} — over a bit-exact parallel objective the constrained trajectory is bit-identical
// across worker counts (NON-VACUOUS: ≥1 variable is pinned at a bound at the solution).

#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace opt = crd::hesap::opt;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;
constexpr crd::f64 kInf = 1e30; // L-BFGS-B "no bound" sentinel

Csr well_conditioned_spd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 1.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -0.1);
            tb.add(i + 1, i, -0.1);
        }
    }
    return tb.compress();
}

// Extended Rosenbrock with analytic gradient.
class RosenbrockObj final : public opt::Objective<crd::f64>
{
public:
    explicit RosenbrockObj(crd::usize n)
        : opt::Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false), m_n(n)
    {
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 f = 0.0;
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const crd::f64 a = 1.0 - x[i];
            const crd::f64 b = x[i + 1] - x[i] * x[i];
            f += a * a + 100.0 * b * b;
        }
        return f;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            g[i] = 0.0;
        }
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const crd::f64 a = 1.0 - x[i];
            const crd::f64 b = x[i + 1] - x[i] * x[i];
            g[i] += -2.0 * a - 400.0 * x[i] * b;
            g[i + 1] += 200.0 * b;
        }
        return true;
    }

private:
    crd::usize m_n;
};
} // namespace

TEST_CASE("v7-d-3 L-BFGS-B converges on bounded Rosenbrock with active bounds", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 6;
    RosenbrockObj obj(n);

    crd::containers::Array<crd::f64> x0(&alloc);
    crd::containers::Array<crd::f64> lo(&alloc);
    crd::containers::Array<crd::f64> hi(&alloc);
    x0.resize(n);
    lo.resize(n);
    hi.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x0[i] = (i % 2 == 0) ? -1.2 : 1.0;
        lo[i] = -2.0;
        hi[i] = 0.5; // unconstrained min x*=1 is INFEASIBLE ⇒ bounds bind
    }

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-6;
    opts.max_iters = 1000;
    auto r = opt::minimize_lbfgsb<crd::f64>(obj, {x0.data(), n}, {lo.data(), n}, {hi.data(), n}, opts, &alloc, 5);

    REQUIRE(r.status == opt::OptStatus::Success);
    bool pinned = false;
    for (crd::usize i = 0; i < n; ++i)
    {
        CHECK(r.x[i] >= lo[i] - 1e-10); // feasible
        CHECK(r.x[i] <= hi[i] + 1e-10);
        if (std::fabs(r.x[i] - hi[i]) < 1e-7 || std::fabs(r.x[i] - lo[i]) < 1e-7)
        {
            pinned = true;
        }
    }
    CHECK(pinned); // ≥1 bound active at the constrained minimizer
}

TEST_CASE("v7-d-3 L-BFGS-B with no bounds reduces to unconstrained L-BFGS (Rosenbrock)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 8;
    RosenbrockObj obj(n);

    crd::containers::Array<crd::f64> x0(&alloc);
    crd::containers::Array<crd::f64> lo(&alloc);
    crd::containers::Array<crd::f64> hi(&alloc);
    x0.resize(n);
    lo.resize(n);
    hi.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x0[i] = (i % 2 == 0) ? -1.2 : 1.0;
        lo[i] = -kInf; // unbounded both sides ⇒ nbd = 0
        hi[i] = kInf;
    }

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-6;
    opts.max_iters = 2000;
    auto r = opt::minimize_lbfgsb<crd::f64>(obj, {x0.data(), n}, {lo.data(), n}, {hi.data(), n}, opts, &alloc, 8);

    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - 1.0)); // unconstrained min = all ones
    }
    CHECK(err < 1e-4);
}

TEST_CASE("v7-d-3 L-BFGS-B determinism moat {1,2,4,8,16} (active bounds, parallel objective)",
          "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 32;
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr a = well_conditioned_spd(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    crd::containers::Array<crd::f64> lo(&alloc);
    crd::containers::Array<crd::f64> hi(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x0.resize(n);
    lo.resize(n);
    hi.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 0.5 + 0.1 * static_cast<crd::f64>(i); // ranges 0.5 .. ~3.6
        x0[i] = 0.0;
        lo[i] = 0.0;
        hi[i] = 2.0; // unconstrained min (xtrue) exceeds 2 for the upper half ⇒ those pin to 2
    }
    (void)serial_op.apply({xtrue.data(), n}, {b.data(), n}); // b = A·xtrue ⇒ unconstrained x* = xtrue

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 1000;

    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    crd::usize iters_ref = 0;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);
            auto r = opt::minimize_lbfgsb<crd::f64>(obj, {x0.data(), n}, {lo.data(), n}, {hi.data(), n}, opts,
                                                    &alloc, 5);
            REQUIRE(r.status == opt::OptStatus::Success); // converged on the PROJECTED gradient (the real cert)
            REQUIRE(r.iterations > 1); // multi-iteration trajectory ⇒ the moat isn't a one-step coincidence (v6 trap)
            bool pinned = false;
            for (crd::u32 i = 0; i < n; ++i)
            {
                if (std::fabs(r.x[i] - 2.0) < 1e-7)
                {
                    pinned = true;
                }
            }
            REQUIRE(pinned); // bounds genuinely active ⇒ the GCP/subspace machinery is exercised (non-vacuous)
            if (!have_ref)
            {
                x_ref.resize(r.x.size());
                for (crd::u32 i = 0; i < r.x.size(); ++i)
                {
                    x_ref[i] = r.x[i];
                }
                iters_ref = r.iterations;
                have_ref = true;
            }
            else
            {
                bool ident = (r.iterations == iters_ref) && (r.x.size() == x_ref.size());
                for (crd::u32 i = 0; i < r.x.size() && ident; ++i)
                {
                    ident = (r.x[i] == x_ref[i]);
                }
                CHECK(ident); // bound-constrained trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
