// crd-hesap-opt v7-h — trust-region framework + the subproblem ladder. Validates: (1) the SHARP exact-subproblem
// gold-check — the Moré-Sorensen KKT certificate ((H+λI)p = −g, λ ≥ max(0,−λ₁), λ·(Δ−‖p‖) = 0) verified
// DIRECTLY on interior / boundary / indefinite / HARD-CASE (g ⊥ the λ₁-eigenspace) / pure-saddle (g = 0)
// instances with known spectra; (2) the driver converges with ALL SIX subproblems on an SPD quadratic (incl.
// Cauchy, the theory floor); (3) Rosenbrock-2 with the five Newton-class subproblems (TR globalization); (4)
// saddle escape from an indefinite start (Steihaug negative-curvature exit · GLTR · exact); (5) the determinism
// moat {1,2,4,8,16} — Steihaug over the parallel-but-bit-exact spmv; (6) boundary n = 0.

#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;
namespace sp = crd::hesap::sparse;
namespace dn = crd::hesap::dense;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

Csr laplacian_1d(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 2.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
            tb.add(i + 1, i, -1.0);
        }
    }
    return tb.compress();
}

// Dense SPD quadratic f = ½(x−c)ᵀA(x−c) — f* = 0 EXACTLY at c, so the f64 resolution floor (eps·|f|) scales
// away near the minimum and even the slow Cauchy path can certify tight gradient tolerances (the v7-g lesson).
// Every second-order capability (one class serves all six subproblems).
class QuadAll final : public opt::Objective<crd::f64>
{
public:
    QuadAll(crd::containers::ConstSpan<crd::f64> a, crd::containers::ConstSpan<crd::f64> c, crd::usize n) noexcept
        : Objective<crd::f64>(true, /*has_hessian_vector=*/true, /*has_hessian=*/true), m_a(a), m_c(c), m_n(n)
    {
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_n; ++i)
        {
            crd::f64 ax = 0.0;
            for (crd::usize j = 0; j < m_n; ++j)
            {
                ax += m_a[i * m_n + j] * (x[j] - m_c[j]);
            }
            acc += 0.5 * (x[i] - m_c[i]) * ax;
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            crd::f64 ax = 0.0;
            for (crd::usize j = 0; j < m_n; ++j)
            {
                ax += m_a[i * m_n + j] * (x[j] - m_c[j]);
            }
            g[i] = ax;
        }
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64> h) const override
    {
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            h[k] = m_a[k];
        }
        return true;
    }
    [[nodiscard]] bool hessian_vector(crd::containers::ConstSpan<crd::f64>, crd::containers::ConstSpan<crd::f64> v,
                                      crd::containers::Span<crd::f64> hv) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            crd::f64 acc = 0.0;
            for (crd::usize j = 0; j < m_n; ++j)
            {
                acc += m_a[i * m_n + j] * v[j];
            }
            hv[i] = acc;
        }
        return true;
    }

private:
    crd::containers::ConstSpan<crd::f64> m_a;
    crd::containers::ConstSpan<crd::f64> m_c;
    crd::usize m_n;
};

// Rosenbrock-2 (single global minimizer — see the v7-g local-minimizer lesson) with all capabilities.
class Rosen2All final : public opt::Objective<crd::f64>
{
public:
    Rosen2All() noexcept : Objective<crd::f64>(true, /*has_hessian_vector=*/true, /*has_hessian=*/true) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::f64 a = 1.0 - x[0];
        const crd::f64 b = x[1] - x[0] * x[0];
        return a * a + 100.0 * b * b;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        const crd::f64 b = x[1] - x[0] * x[0];
        g[0] = -2.0 * (1.0 - x[0]) - 400.0 * x[0] * b;
        g[1] = 200.0 * b;
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        h[0] = 2.0 + 1200.0 * x[0] * x[0] - 400.0 * x[1];
        h[1] = -400.0 * x[0];
        h[2] = -400.0 * x[0];
        h[3] = 200.0;
        return true;
    }
    [[nodiscard]] bool hessian_vector(crd::containers::ConstSpan<crd::f64> x, crd::containers::ConstSpan<crd::f64> v,
                                      crd::containers::Span<crd::f64> hv) const override
    {
        const crd::f64 h00 = 2.0 + 1200.0 * x[0] * x[0] - 400.0 * x[1];
        const crd::f64 h01 = -400.0 * x[0];
        hv[0] = h00 * v[0] + h01 * v[1];
        hv[1] = h01 * v[0] + 200.0 * v[1];
        return true;
    }
};

// Double well in x₀ + bowl in the rest (Hessian indefinite for |x₀| < 1/√3) with all capabilities.
class DoubleWellAll final : public opt::Objective<crd::f64>
{
public:
    explicit DoubleWellAll(crd::usize n) noexcept
        : Objective<crd::f64>(true, /*has_hessian_vector=*/true, /*has_hessian=*/true), m_n(n)
    {
    }
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.25 * x[0] * x[0] * x[0] * x[0] - 0.5 * x[0] * x[0];
        for (crd::usize i = 1; i < m_n; ++i)
        {
            acc += 0.5 * x[i] * x[i];
        }
        return acc;
    }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        g[0] = x[0] * x[0] * x[0] - x[0];
        for (crd::usize i = 1; i < m_n; ++i)
        {
            g[i] = x[i];
        }
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            h[k] = 0.0;
        }
        h[0] = 3.0 * x[0] * x[0] - 1.0;
        for (crd::usize i = 1; i < m_n; ++i)
        {
            h[i * m_n + i] = 1.0;
        }
        return true;
    }
    [[nodiscard]] bool hessian_vector(crd::containers::ConstSpan<crd::f64> x, crd::containers::ConstSpan<crd::f64> v,
                                      crd::containers::Span<crd::f64> hv) const override
    {
        hv[0] = (3.0 * x[0] * x[0] - 1.0) * v[0];
        for (crd::usize i = 1; i < m_n; ++i)
        {
            hv[i] = v[i];
        }
        return true;
    }

private:
    crd::usize m_n;
};

class EmptyAll final : public opt::Objective<crd::f64>
{
public:
    EmptyAll() noexcept : Objective<crd::f64>(true, true, true, false) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64>) const override { return 0.0; }
    [[nodiscard]] crd::usize n() const noexcept override { return 0; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>) const override
    {
        return true;
    }
    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>) const override
    {
        return true;
    }
};

// KKT certificate for min gᵀp + ½pᵀHp s.t. ‖p‖ ≤ Δ given the KNOWN λ₁ of a diagonal H: residual of
// (H+λI)p = −g, λ ≥ max(0, −λ₁) (H+λI ⪰ 0), and complementarity λ·(Δ−‖p‖) = 0.
void check_kkt_diag(crd::containers::ConstSpan<crd::f64> hdiag, crd::containers::ConstSpan<crd::f64> g,
                    crd::containers::ConstSpan<crd::f64> p, crd::f64 lambda, crd::f64 delta, crd::f64 lam1)
{
    const crd::usize n = hdiag.size();
    crd::f64 resid = 0.0;
    crd::f64 pnorm = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        resid = std::max(resid, std::fabs((hdiag[i] + lambda) * p[i] + g[i]));
        pnorm += p[i] * p[i];
    }
    pnorm = std::sqrt(pnorm);
    CHECK(resid < 1e-9);                                     // stationarity
    CHECK(lambda >= std::max(0.0, -lam1) - 1e-10);           // dual feasibility / H+λI ⪰ 0
    CHECK(pnorm <= delta * (1.0 + 1e-9));                    // primal feasibility
    CHECK(lambda * (delta - pnorm) < 1e-8 * (1.0 + lambda)); // complementarity
}
} // namespace

TEST_CASE("v7-h exact subproblem: the More-Sorensen KKT certificate (incl. the hard case)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 3;
    crd::containers::Array<crd::f64> p(&alloc);
    p.resize(n);

    SECTION("interior (PD, large delta): lambda = 0, Hp = -g")
    {
        dn::Symmetric<crd::f64> h(&alloc, n);
        h.at(0, 0) = 1.0;
        h.at(1, 1) = 2.0;
        h.at(2, 2) = 5.0;
        const crd::f64 hdiag[] = {1.0, 2.0, 5.0};
        const crd::f64 g[] = {1.0, -2.0, 0.5};
        const auto r = opt::solve_trust_region_subproblem_exact<crd::f64>(&alloc, h, {g, n}, 100.0, {p.data(), n});
        CHECK(r.lambda == 0.0);
        CHECK_FALSE(r.hits_boundary);
        check_kkt_diag({hdiag, n}, {g, n}, {p.data(), n}, r.lambda, 100.0, 1.0);
        CHECK(r.pred > 0.0);
    }
    SECTION("boundary (PD, small delta): lambda > 0, the norm pinned to delta")
    {
        dn::Symmetric<crd::f64> h(&alloc, n);
        h.at(0, 0) = 1.0;
        h.at(1, 1) = 2.0;
        h.at(2, 2) = 5.0;
        const crd::f64 hdiag[] = {1.0, 2.0, 5.0};
        const crd::f64 g[] = {1.0, -2.0, 0.5};
        const crd::f64 delta = 0.1;
        const auto r = opt::solve_trust_region_subproblem_exact<crd::f64>(&alloc, h, {g, n}, delta, {p.data(), n});
        CHECK(r.lambda > 0.0);
        CHECK(r.hits_boundary);
        check_kkt_diag({hdiag, n}, {g, n}, {p.data(), n}, r.lambda, delta, 1.0);
    }
    SECTION("indefinite regular: lambda >= -lam1, boundary")
    {
        dn::Symmetric<crd::f64> h(&alloc, n);
        h.at(0, 0) = -2.0;
        h.at(1, 1) = 1.0;
        h.at(2, 2) = 3.0;
        const crd::f64 hdiag[] = {-2.0, 1.0, 3.0};
        const crd::f64 g[] = {1.0, 1.0, 1.0}; // g has a component on the lam1 eigenvector — regular case
        const crd::f64 delta = 1.0;
        const auto r = opt::solve_trust_region_subproblem_exact<crd::f64>(&alloc, h, {g, n}, delta, {p.data(), n});
        CHECK(r.hits_boundary);
        check_kkt_diag({hdiag, n}, {g, n}, {p.data(), n}, r.lambda, delta, -2.0);
    }
    SECTION("HARD case: g perpendicular to the lam1 eigenspace, boundary pad along it")
    {
        dn::Symmetric<crd::f64> h(&alloc, n);
        h.at(0, 0) = -2.0;
        h.at(1, 1) = 1.0;
        h.at(2, 2) = 3.0;
        const crd::f64 hdiag[] = {-2.0, 1.0, 3.0};
        const crd::f64 g[] = {0.0, 1.0, 1.0}; // ⊥ e1; limit norm = √((1/3)²+(1/5)²) ≈ 0.389 < Δ ⇒ HARD
        const crd::f64 delta = 1.0;
        const auto r = opt::solve_trust_region_subproblem_exact<crd::f64>(&alloc, h, {g, n}, delta, {p.data(), n});
        CHECK(r.hits_boundary);
        CHECK(std::fabs(r.lambda - 2.0) < 1e-7); // λ = −λ₁ exactly in the hard case
        check_kkt_diag({hdiag, n}, {g, n}, {p.data(), n}, r.lambda, delta, -2.0);
        CHECK(std::fabs(std::fabs(p[0]) - std::sqrt(1.0 - (1.0 / 9.0 + 1.0 / 25.0))) < 1e-7); // the τ pad
    }
    SECTION("pure saddle (g = 0, indefinite): boundary step along the most-negative direction")
    {
        dn::Symmetric<crd::f64> h(&alloc, n);
        h.at(0, 0) = -2.0;
        h.at(1, 1) = 1.0;
        h.at(2, 2) = 3.0;
        const crd::f64 g[] = {0.0, 0.0, 0.0};
        const crd::f64 delta = 0.5;
        const auto r = opt::solve_trust_region_subproblem_exact<crd::f64>(&alloc, h, {g, n}, delta, {p.data(), n});
        CHECK(r.hits_boundary);
        CHECK(std::fabs(std::fabs(p[0]) - delta) < 1e-9);            // ±Δ·q₁
        CHECK(std::fabs(r.pred - 0.5 * 2.0 * delta * delta) < 1e-9); // −m = ½·|λ₁|·Δ²
    }
}

TEST_CASE("v7-h trust-region driver: all six subproblems solve an SPD quadratic", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 16;
    crd::containers::Array<crd::f64> a(&alloc);
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    a.resize(n * n);
    xtrue.resize(n);
    x0.resize(n);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        a[k] = 0.0;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        a[i * n + i] = 2.0;
        if (i + 1 < n)
        {
            a[i * n + (i + 1)] = -1.0;
            a[(i + 1) * n + i] = -1.0;
        }
        xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    const QuadAll obj({a.data(), n * n}, {xtrue.data(), n}, n);

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 20000; // Cauchy is steepest-descent-class (κ ≈ 58 here) — the others finish in ≤ ~15

    for (auto sub : {opt::TrustRegionSubproblem::Cauchy, opt::TrustRegionSubproblem::Dogleg,
                     opt::TrustRegionSubproblem::Subspace2D, opt::TrustRegionSubproblem::SteihaugCg,
                     opt::TrustRegionSubproblem::TrustKrylov, opt::TrustRegionSubproblem::Exact})
    {
        opt::TrustRegionOptions<crd::f64> tr;
        tr.subproblem = sub;
        auto r = opt::minimize_trust_region<crd::f64>(obj, {x0.data(), n}, opts, &alloc, tr);
        REQUIRE(r.status == opt::OptStatus::Success);
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
        }
        CHECK(err < 1e-6);
        CHECK(r.fn_evals > 0);
        CHECK(r.hess_evals > 0);
        if (sub != opt::TrustRegionSubproblem::Cauchy)
        {
            CHECK(r.iterations < 60); // Newton-class subproblems: a handful of outer iterations
        }
    }
}

TEST_CASE("v7-h trust-region: Rosenbrock-2 with the Newton-class subproblems", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const Rosen2All obj;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = -1.2;
    x0[1] = 1.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 500;

    for (auto sub : {opt::TrustRegionSubproblem::Dogleg, opt::TrustRegionSubproblem::Subspace2D,
                     opt::TrustRegionSubproblem::SteihaugCg, opt::TrustRegionSubproblem::TrustKrylov,
                     opt::TrustRegionSubproblem::Exact})
    {
        opt::TrustRegionOptions<crd::f64> tr;
        tr.subproblem = sub;
        auto r = opt::minimize_trust_region<crd::f64>(obj, {x0.data(), 2}, opts, &alloc, tr);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(r.x[0] - 1.0) < 1e-6);
        CHECK(std::fabs(r.x[1] - 1.0) < 1e-6);
        CHECK(r.fx < 1e-12);
        CHECK(r.iterations < 300);
    }
}

TEST_CASE("v7-h trust-region: saddle escape from an indefinite start", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 4;
    const DoubleWellAll obj(dim);
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(dim);
    x0[0] = 0.05; // indefinite region (and ∇f ≈ 0 in x₀ — the TR negative-curvature machinery must dig out)
    for (crd::usize i = 1; i < dim; ++i)
    {
        x0[i] = 1.0;
    }
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 500;

    for (auto sub : {opt::TrustRegionSubproblem::SteihaugCg, opt::TrustRegionSubproblem::TrustKrylov,
                     opt::TrustRegionSubproblem::Exact})
    {
        opt::TrustRegionOptions<crd::f64> tr;
        tr.subproblem = sub;
        auto r = opt::minimize_trust_region<crd::f64>(obj, {x0.data(), dim}, opts, &alloc, tr);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(std::fabs(r.x[0]) - 1.0) < 1e-7); // a TRUE minimizer (either well), not the saddle
        CHECK(std::fabs(r.fx + 0.25) < 1e-10);
    }
}

TEST_CASE("v7-h trust-region determinism moat {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 64; // κ ~ 1700 ⇒ many Steihaug inner + several outer iterations (moat non-vacuous)
    crd::memory::TlsfAllocator alloc(1U << 25);
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x0.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.02 * static_cast<crd::f64>(i);
        x0[i] = 0.0;
    }
    (void)serial_op.apply({xtrue.data(), n}, {b.data(), n});

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;
    opt::TrustRegionOptions<crd::f64> tr;
    tr.subproblem = opt::TrustRegionSubproblem::SteihaugCg;

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
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc); // gradient + H·v = parallel spmv
            auto r = opt::minimize_trust_region<crd::f64>(obj, {x0.data(), n}, opts, &alloc, tr);

            REQUIRE(r.status == opt::OptStatus::Success);
            REQUIRE(r.iterations > 1); // genuinely iterative ⇒ moat not vacuous
            if (!have_ref)
            {
                x_ref.resize(r.x.size());
                for (crd::usize i = 0; i < r.x.size(); ++i)
                {
                    x_ref[i] = r.x[i];
                }
                iters_ref = r.iterations;
                have_ref = true;
            }
            else
            {
                bool ident = (r.iterations == iters_ref) && (r.x.size() == x_ref.size());
                for (crd::usize i = 0; i < r.x.size() && ident; ++i)
                {
                    ident = (r.x[i] == x_ref[i]);
                }
                CHECK(ident); // trust-region trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-h trust-region boundary: n = 0", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const EmptyAll obj;
    opt::OptOptions<crd::f64> opts;
    for (auto sub : {opt::TrustRegionSubproblem::SteihaugCg, opt::TrustRegionSubproblem::Exact})
    {
        opt::TrustRegionOptions<crd::f64> tr;
        tr.subproblem = sub;
        auto r =
            opt::minimize_trust_region<crd::f64>(obj, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc, tr);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(r.x.size() == 0);
    }
}
