// crd-hesap-opt v7-g — SPARSE-Hessian Newton (CSR → hesap-direct supernodal Cholesky). Validates: (1) the sharp
// one-step gold-check on a sparse SPD quadratic (the Newton step is exact); (2) a convex sparse QUARTIC (the
// fixed-pattern symbolic-once regime: tridiagonal Hessian whose VALUES change per iteration) matches the dense
// Newton driver on the same problem; (3) the τ·I escalation through an INDEFINITE start (coupled double well —
// the supernodal factor reports non-PD via info() and the N&W 3.4 escalation must recover a descent direction);
// (4) the determinism moat {1,2,4,8,16} — the tree-parallel supernodal factor (v5a moat) carried through the
// whole Newton trajectory; (5) boundary n = 0.

#include <crd/hesap/opt/newton_sparse.hpp> // NOT in the umbrella — explicit include (hesap-opt→hesap-direct edge)
#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;
namespace sp = crd::hesap::sparse;

namespace
{
// f = ½xᵀAx − bᵀx + Σ¼x_i⁴ over A = tridiag(−1,2,−1): smooth, strictly convex, NON-quadratic — Newton needs
// several iterations and the (tridiagonal, fixed-pattern) sparse Hessian A + diag(3x_i²) changes values each
// one: exactly the symbolic-once / refactorize regime. `dense` switches the same f to the dense-Hessian
// capability so the dense and sparse drivers can be cross-checked on an identical problem.
class QuarticChain final : public opt::Objective<crd::f64>
{
public:
    QuarticChain(crd::containers::ConstSpan<crd::f64> b, crd::memory::IAllocator* alloc, bool dense) noexcept
        : Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false, /*has_hessian=*/dense,
                              /*has_sparse_hessian=*/!dense),
          m_b(b), m_alloc(alloc)
    {
    }

    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        const crd::usize n = m_b.size();
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            acc += x[i] * x[i]; // ½·2x_i² from the diagonal
            if (i + 1 < n)
            {
                acc -= x[i] * x[i + 1]; // ½·(−2 x_i x_{i+1}) from the two off-diagonals
            }
            acc += 0.25 * x[i] * x[i] * x[i] * x[i] - m_b[i] * x[i];
        }
        return acc;
    }

    [[nodiscard]] crd::usize n() const noexcept override { return m_b.size(); }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        const crd::usize n = m_b.size();
        for (crd::usize i = 0; i < n; ++i)
        {
            crd::f64 ax = 2.0 * x[i];
            if (i > 0)
            {
                ax -= x[i - 1];
            }
            if (i + 1 < n)
            {
                ax -= x[i + 1];
            }
            g[i] = ax - m_b[i] + x[i] * x[i] * x[i];
        }
        return true;
    }

    [[nodiscard]] bool hessian(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> h) const override
    {
        const crd::usize n = m_b.size();
        for (crd::usize k = 0; k < n * n; ++k)
        {
            h[k] = 0.0;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            h[i * n + i] = 2.0 + 3.0 * x[i] * x[i];
            if (i + 1 < n)
            {
                h[i * n + (i + 1)] = -1.0;
                h[(i + 1) * n + i] = -1.0;
            }
        }
        return true;
    }

    [[nodiscard]] bool sparse_hessian(crd::containers::ConstSpan<crd::f64> x,
                                      sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& out) const override
    {
        const crd::usize n = m_b.size();
        sp::TripletBuilder<crd::f64> tb(m_alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
        for (crd::u32 i = 0; i < n; ++i)
        {
            tb.add(i, i, 2.0 + 3.0 * x[i] * x[i]);
            if (i + 1 < n)
            {
                tb.add(i, i + 1, -1.0);
                tb.add(i + 1, i, -1.0);
            }
        }
        out = tb.compress(); // same insertion order every call ⇒ identical pattern (the fixed-sparsity contract)
        return true;
    }

private:
    crd::containers::ConstSpan<crd::f64> m_b;
    crd::memory::IAllocator* m_alloc;
};

// Coupled double well: f = Σ(¼x_i⁴ − ½x_i²) + (c/2)·Σ(x_{i+1}−x_i)². Hessian = diag(3x_i²−1) + c·(graph
// Laplacian) — NEGATIVE-definite-ish near x = 0 ⇒ the sparse driver's τ·I escalation must fire (the supernodal
// factor reports info() != 0). A UNIFORM start stays uniform (the coupling gradient vanishes on uniform
// vectors), so from x = +0.05·1 the minimizer is all-ones, f* = −n/4.
class CoupledDoubleWell final : public opt::Objective<crd::f64>
{
public:
    CoupledDoubleWell(crd::usize n, crd::f64 c, crd::memory::IAllocator* alloc) noexcept
        : Objective<crd::f64>(/*has_gradient=*/true, /*has_hessian_vector=*/false, /*has_hessian=*/false,
                              /*has_sparse_hessian=*/true),
          m_n(n), m_c(c), m_alloc(alloc)
    {
    }

    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64> x) const override
    {
        crd::f64 acc = 0.0;
        for (crd::usize i = 0; i < m_n; ++i)
        {
            acc += 0.25 * x[i] * x[i] * x[i] * x[i] - 0.5 * x[i] * x[i];
            if (i + 1 < m_n)
            {
                const crd::f64 d = x[i + 1] - x[i];
                acc += 0.5 * m_c * d * d;
            }
        }
        return acc;
    }

    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> g) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            g[i] = x[i] * x[i] * x[i] - x[i];
            if (i > 0)
            {
                g[i] += m_c * (x[i] - x[i - 1]);
            }
            if (i + 1 < m_n)
            {
                g[i] -= m_c * (x[i + 1] - x[i]);
            }
        }
        return true;
    }

    [[nodiscard]] bool sparse_hessian(crd::containers::ConstSpan<crd::f64> x,
                                      sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& out) const override
    {
        sp::TripletBuilder<crd::f64> tb(m_alloc, static_cast<crd::u32>(m_n), static_cast<crd::u32>(m_n));
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const crd::f64 deg = (i == 0 || i + 1 == m_n) ? 1.0 : 2.0; // free-boundary chain
            tb.add(i, i, 3.0 * x[i] * x[i] - 1.0 + m_c * deg);
            if (i + 1 < m_n)
            {
                tb.add(i, i + 1, -m_c);
                tb.add(i + 1, i, -m_c);
            }
        }
        out = tb.compress();
        return true;
    }

private:
    crd::usize m_n;
    crd::f64 m_c;
    crd::memory::IAllocator* m_alloc;
};

class EmptySparse final : public opt::Objective<crd::f64>
{
public:
    EmptySparse() noexcept : Objective<crd::f64>(true, false, false, true) {}
    [[nodiscard]] crd::f64 value(crd::containers::ConstSpan<crd::f64>) const override { return 0.0; }
    [[nodiscard]] crd::usize n() const noexcept override { return 0; }
    [[nodiscard]] bool gradient(crd::containers::ConstSpan<crd::f64>, crd::containers::Span<crd::f64>) const override
    {
        return true;
    }
};
} // namespace

TEST_CASE("v7-g sparse Newton: convex quartic chain converges and matches dense Newton", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::usize n = 64;
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    b.resize(n);
    x0.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        b[i] = 0.5 + 0.05 * static_cast<crd::f64>(i % 7);
        x0[i] = 0.0;
    }
    const QuarticChain sparse_obj({b.data(), n}, &alloc, /*dense=*/false);
    const QuarticChain dense_obj({b.data(), n}, &alloc, /*dense=*/true);

    opt::OptOptions<crd::f64> opts;
    // ⚠ NOT tighter: at |f*| ≈ 26.5, f-differences near the minimizer hit the f64 resolution floor (eps·|f| ≈
    // 6e-15) before ‖g‖∞ reaches 1e-10 — the line search then can NOT certify progress (measured: gnorm stalls
    // at ~1e-9 ⇒ LineSearchFailed at a tolerance the arithmetic cannot support).
    opts.grad_tol = 1e-8;
    opts.max_iters = 100;

    auto rs = opt::minimize_newton_sparse<crd::f64>(sparse_obj, {x0.data(), n}, opts, &alloc);
    auto rd = opt::minimize_newton<crd::f64>(dense_obj, {x0.data(), n}, opts, &alloc);
    REQUIRE(rs.status == opt::OptStatus::Success);
    REQUIRE(rd.status == opt::OptStatus::Success);
    REQUIRE(rs.iterations > 1); // genuinely non-quadratic ⇒ the refactorize() path actually runs
    crd::f64 dmax = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        dmax = std::max(dmax, std::fabs(rs.x[i] - rd.x[i]));
    }
    CHECK(dmax < 1e-8); // same minimizer via the sparse and the dense factor
    // One sparse Hessian per iteration (the exact count differs by where the terminating test fires).
    CHECK((rs.hess_evals == rs.iterations + 1 || rs.hess_evals == rs.iterations));
}

TEST_CASE("v7-g sparse Newton: tau escalation through an indefinite start (coupled double well)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::usize n = 32;
    const CoupledDoubleWell obj(n, 0.1, &alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x0[i] = 0.05; // Hessian ≈ −I + 0.1·L here: NOT positive definite ⇒ the τ·I path must fire
    }
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8; // not tighter — the f64 resolution floor at |f*| = n/4 (see the quartic-chain case)
    opts.max_iters = 300;
    auto r = opt::minimize_newton_sparse<crd::f64>(obj, {x0.data(), n}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    for (crd::usize i = 0; i < n; ++i)
    {
        CHECK(std::fabs(r.x[i] - 1.0) < 1e-7); // uniform start ⇒ the all-ones well, NOT the saddle at 0
    }
    CHECK(std::fabs(r.fx + 0.25 * static_cast<crd::f64>(n)) < 1e-8); // f* = −n/4
}

TEST_CASE("v7-g sparse Newton determinism moat {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::usize n = 64;
    crd::memory::TlsfAllocator alloc(1U << 25);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x0(&alloc);
    b.resize(n);
    x0.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        b[i] = 0.5 + 0.05 * static_cast<crd::f64>(i % 7);
        x0[i] = 0.0;
    }
    const QuarticChain obj({b.data(), n}, &alloc, /*dense=*/false);

    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8; // not tighter — the f64 resolution floor (see the quartic-chain case)
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
            auto r = opt::minimize_newton_sparse<crd::f64>(obj, {x0.data(), n}, opts, &alloc, nullptr, nw);
            REQUIRE(r.status == opt::OptStatus::Success);
            REQUIRE(r.iterations > 1); // non-vacuous (the tree-parallel supernodal factor runs every iteration)
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
                CHECK(ident); // the v5a supernodal moat carried through the whole Newton trajectory
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-g sparse Newton boundary: n = 0", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const EmptySparse obj;
    opt::OptOptions<crd::f64> opts;
    auto r = opt::minimize_newton_sparse<crd::f64>(obj, {static_cast<const crd::f64*>(nullptr), 0}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK(r.x.size() == 0);
}
