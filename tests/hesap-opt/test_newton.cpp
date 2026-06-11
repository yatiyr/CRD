// crd-hesap-opt v7-g — Newton family (dense full/modified Newton + Newton-CG). Validates: (1) the SHARP
// full-Newton gold-check — on an SPD QUADRATIC the Newton step is EXACT, so it converges in ONE iteration with
// ONE Hessian evaluation (α=1 accepted; any modification/solve bug breaks this); (2) Rosenbrock-N with the
// analytic Hessian (line-search globalization of the pure method); (3) the modified-Newton NEGATIVE-CURVATURE
// gate — from inside the indefinite region of a double well, pure Newton heads to the SADDLE; the N&W 3.4 τ·I
// device must escape to a true minimizer (f = −¼, |x₀| = 1); (4) Newton-CG: same three properties matrix-free
// (forcing-sequence truncation + the inner negative-curvature exit); (5) the Newton-CG determinism moat
// {1,2,4,8,16} — gradient AND Hessian-vector both ride the parallel-but-bit-exact spmv; (6) boundary n = 0.

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

// Dense SPD quadratic f = ½xᵀAx − bᵀx with an analytic dense Hessian (A itself) — the one-step Newton vehicle.
class DenseQuad final : public opt::Objective<crd::f64>
{
public:
    DenseQuad(crd::containers::ConstSpan<crd::f64> a, crd::containers::ConstSpan<crd::f64> b, crd::usize n) noexcept
        : Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false, /*has_hessian=*/true), m_a(a),
          m_b(b), m_n(n)
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
                ax += m_a[i * m_n + j] * x[j];
            }
            acc += 0.5 * x[i] * ax - m_b[i] * x[i];
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
                ax += m_a[i * m_n + j] * x[j];
            }
            g[i] = ax - m_b[i];
        }
        return true;
    }

    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        (void)x; // constant Hessian = A
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            h[k] = m_a[k];
        }
        return true;
    }

private:
    crd::containers::ConstSpan<crd::f64> m_a;
    crd::containers::ConstSpan<crd::f64> m_b;
    crd::usize m_n;
};

// Extended Rosenbrock with the analytic gradient, dense Hessian (tridiagonal), and Hessian-vector product.
class RosenbrockNewton final : public opt::Objective<crd::f64>
{
public:
    explicit RosenbrockNewton(crd::usize n) noexcept
        : Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/true, /*has_hessian=*/true), m_n(n)
    {
    }

    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const crd::f64 a = 1.0 - x[i];
            const crd::f64 b = x[i + 1] - x[i] * x[i];
            acc += a * a + 100.0 * b * b;
        }
        return acc;
    }

    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            g[i] = 0.0;
        }
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const crd::f64 b = x[i + 1] - x[i] * x[i];
            g[i] += -2.0 * (1.0 - x[i]) - 400.0 * x[i] * b;
            g[i + 1] += 200.0 * b;
        }
        return true;
    }

    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            h[k] = 0.0;
        }
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            h[i * m_n + i] += 2.0 + 1200.0 * x[i] * x[i] - 400.0 * x[i + 1];
            h[i * m_n + (i + 1)] += -400.0 * x[i];
            h[(i + 1) * m_n + i] += -400.0 * x[i];
            h[(i + 1) * m_n + (i + 1)] += 200.0;
        }
        return true;
    }

    [[nodiscard]] bool hessian_vector(crd::containers::ConstSpan<crd::f64> x, crd::containers::ConstSpan<crd::f64> v,
                                      crd::containers::Span<crd::f64> hv) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            hv[i] = 0.0;
        }
        for (crd::usize i = 0; i + 1 < m_n; ++i)
        {
            const crd::f64 hii = 2.0 + 1200.0 * x[i] * x[i] - 400.0 * x[i + 1];
            const crd::f64 hio = -400.0 * x[i];
            hv[i] += hii * v[i] + hio * v[i + 1];
            hv[i + 1] += hio * v[i] + 200.0 * v[i + 1];
        }
        return true;
    }

private:
    crd::usize m_n;
};

// Double well in x₀ + quadratic bowl in the rest: f = ¼x₀⁴ − ½x₀² + ½Σ_{i≥1}x_i². Hessian = diag(3x₀²−1, 1, …)
// — INDEFINITE for |x₀| < 1/√3 (pure Newton heads to the saddle x₀ = 0; modified Newton must reach |x₀| = 1,
// f* = −¼). The negative-curvature gate for both the dense τ·I device and the Newton-CG inner exit.
class DoubleWell final : public opt::Objective<crd::f64>
{
public:
    explicit DoubleWell(crd::usize n) noexcept
        : Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/true, /*has_hessian=*/true), m_n(n)
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

// All capabilities claimed, n = 0 — the boundary vehicle (the drivers must early-return Success).
class EmptyObjective final : public opt::Objective<crd::f64>
{
public:
    EmptyObjective() noexcept : Objective<crd::f64>(true, true, true, true) {}
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
} // namespace

TEST_CASE("v7-g Newton: one-step convergence on an SPD quadratic", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 16;
    crd::containers::Array<crd::f64> a(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    a.resize(n * n);
    b.resize(n);
    xtrue.resize(n);
    x0.resize(n);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        a[k] = 0.0;
    }
    for (crd::usize i = 0; i < n; ++i) // dense tridiag(−1,2,−1): SPD
    {
        a[i * n + i] = 2.0;
        if (i + 1 < n)
        {
            a[i * n + (i + 1)] = -1.0;
            a[(i + 1) * n + i] = -1.0;
        }
        xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
        x0[i] = -2.0 + 0.3 * static_cast<crd::f64>(i);
    }
    for (crd::usize i = 0; i < n; ++i) // b = A·xtrue
    {
        crd::f64 s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a[i * n + j] * xtrue[j];
        }
        b[i] = s;
    }
    const DenseQuad obj({a.data(), n * n}, {b.data(), n}, n);

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-9;
    opts.max_iters = 50;
    auto r = opt::minimize_newton<crd::f64>(obj, {x0.data(), n}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(r.iterations <= 1); // the Newton step on a quadratic is EXACT — the sharp full-Newton gold-check
    CHECK(r.hess_evals == 1); // one Hessian, τ = 0 (PD), one factor
    crd::f64 err = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
    }
    CHECK(err < 1e-8);
}

TEST_CASE("v7-g Newton: classic Rosenbrock-2 with the analytic Hessian", "[hesap][opt][v7]")
{
    // ⚠ dim = 2 on PURPOSE: the CHAINED Rosenbrock (n ≥ 4) has a second LOCAL minimizer (x₀ ≈ −1 branch,
    // f ≈ 3.98589 at n = 8) and Newton — an honestly LOCAL method — converges there from the classic
    // alternating start (measured). n = 2 has the single global minimizer (1,1), f* = 0.
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 2;
    const RosenbrockNewton obj(dim);
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(dim);
    x0[0] = -1.2; // the classic hard start
    x0[1] = 1.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;
    auto r = opt::minimize_newton<crd::f64>(obj, {x0.data(), dim}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::usize i = 0; i < dim; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - 1.0));
    }
    CHECK(err < 1e-6);
    CHECK(r.fx < 1e-12);
    CHECK(r.iterations < 100); // Newton-with-line-search territory; a broken τ/solve would blow past this
    CHECK(r.hess_evals > 0);
}

TEST_CASE("v7-g modified Newton escapes the indefinite region (double well)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize dim = 4;
    const DoubleWell obj(dim);
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(dim);
    x0[0] = 0.05; // |x₀| < 1/√3 ⇒ Hessian INDEFINITE here; pure Newton would head to the saddle x₀ = 0
    for (crd::usize i = 1; i < dim; ++i)
    {
        x0[i] = 1.0;
    }
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8; // not tighter — below ~√(eps·|f*|) the line search hits the f64 resolution floor
    opts.max_iters = 200;
    auto r = opt::minimize_newton<crd::f64>(obj, {x0.data(), dim}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(std::fabs(std::fabs(r.x[0]) - 1.0) < 1e-7); // a TRUE minimizer (either well), NOT the saddle
    for (crd::usize i = 1; i < dim; ++i)
    {
        CHECK(std::fabs(r.x[i]) < 1e-7);
    }
    CHECK(std::fabs(r.fx + 0.25) < 1e-10); // f* = −¼
}

TEST_CASE("v7-g Newton-CG: SPD quadratic via hessian_vector", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 64;
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);
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
    (void)op.apply({xtrue.data(), n}, {b.data(), n});
    opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc);

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 100;
    auto r = opt::minimize_newton_cg<crd::f64>(obj, {x0.data(), n}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - xtrue[i]));
    }
    CHECK(err < 1e-6);
    CHECK(r.iterations < 50); // inexact-Newton outer count (forcing sequence ⇒ superlinear), NOT O(κ)
    CHECK(r.hess_evals > 0);  // counts H·v products for Newton-CG
}

TEST_CASE("v7-g Newton-CG: Rosenbrock-N + double-well negative curvature", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 500;

    {
        // dim = 2 (single global minimizer) — chained Rosenbrock n ≥ 4 has a LOCAL min a local method may
        // honestly land in (see the dense-Newton Rosenbrock case).
        const crd::usize dim = 2;
        const RosenbrockNewton obj(dim);
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(dim);
        x0[0] = -1.2;
        x0[1] = 1.0;
        auto r = opt::minimize_newton_cg<crd::f64>(obj, {x0.data(), dim}, opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < dim; ++i)
        {
            err = std::max(err, std::fabs(r.x[i] - 1.0));
        }
        CHECK(err < 1e-6);
    }
    {
        const crd::usize dim = 4;
        const DoubleWell obj(dim);
        crd::containers::Array<crd::f64> x0(&alloc);
        x0.resize(dim);
        x0[0] = 0.05; // indefinite region — exercises the inner CG negative-curvature exit
        for (crd::usize i = 1; i < dim; ++i)
        {
            x0[i] = 1.0;
        }
        opt::OptOptions<crd::f64> dw_opts;
        dw_opts.grad_tol = 1e-8; // not tighter — the f64 resolution floor (see the modified-Newton case)
        dw_opts.max_iters = 500;
        auto r = opt::minimize_newton_cg<crd::f64>(obj, {x0.data(), dim}, dw_opts, &alloc);
        REQUIRE(r.status == opt::OptStatus::Success);
        CHECK(std::fabs(std::fabs(r.x[0]) - 1.0) < 1e-7); // a true minimizer, not the saddle
        CHECK(std::fabs(r.fx + 0.25) < 1e-10);
    }
}

TEST_CASE("v7-g Newton-CG determinism moat {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 64; // κ ~ 1700 ⇒ several outer + many inner iterations (moat non-vacuous)
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
    opts.max_iters = 100;

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
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc); // gradient AND H·v are parallel spmv
            auto r = opt::minimize_newton_cg<crd::f64>(obj, {x0.data(), n}, opts, &alloc);

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
                CHECK(ident); // Newton-CG trajectory bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-g Newton boundary: n = 0", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const EmptyObjective obj;
    opt::OptOptions<crd::f64> opts;

    auto r1 = opt::minimize_newton<crd::f64>(obj, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc);
    REQUIRE(r1.status == opt::OptStatus::Success);
    CHECK(r1.x.size() == 0);

    auto r2 = opt::minimize_newton_cg<crd::f64>(obj, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc);
    REQUIRE(r2.status == opt::OptStatus::Success);
    CHECK(r2.x.size() == 0);
}
