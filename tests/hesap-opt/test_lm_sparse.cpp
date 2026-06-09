// crd-hesap-opt v7-e-2 — sparse-Jacobian Levenberg-Marquardt (the crush vehicle). Validates: (1) sparse-LM
// converges on a nonlinear sparse NLS (chain coupling ⇒ sparse Jacobian, sparse JᵀJ factored by the moat-proven
// hesap-direct supernodal Cholesky) — recovers the known solution, cost→0; (2) it lands at the SAME minimizer as
// the dense LM on the same residual function (sparse path == dense path); (3) the cross-worker {1..16} determinism
// moat — the supernodal factor is bit-identical across worker counts ⇒ the sparse-LM trajectory is too (the
// differentiator Ceres lacks). NON-VACUOUS: iterations>1.

#include <crd/hesap/opt/levenberg_marquardt_sparse.hpp>
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
// Well-conditioned nonlinear sparse chain: r0 = x0−1; r_i = (x_i−1) + 0.3·(sin(x_{i-1})−sin 1)  (i=1..n−1). m=n.
// Solution = all ones (r≡0), cost 0. Diagonal-dominant Jacobian (∂r_i/∂x_i=1, ∂r_i/∂x_{i-1}=0.3·cos(x_{i-1}), |off|
// ≤0.3) ⇒ well-conditioned (unlike x_{i-1}², which compounds geometrically and doesn't pin x as cost→0). Each
// residual touches ≤2 params ⇒ sparse Jacobian (tridiagonal JᵀJ). Provides BOTH dense and sparse Jacobians so the
// sparse path can be cross-checked against the dense LM.
class SparseChain final : public opt::ResidualFunction<crd::f64>
{
public:
    SparseChain(crd::usize n, crd::memory::IAllocator* alloc)
        : opt::ResidualFunction<crd::f64>(/*has_jacobian=*/true, /*has_sparse_jacobian=*/true), m_n(n), m_alloc(alloc)
    {
    }
    void residuals(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> r) const override
    {
        const crd::f64 s1 = std::sin(1.0);
        r[0] = x[0] - 1.0;
        for (crd::usize i = 1; i < m_n; ++i)
        {
            r[i] = (x[i] - 1.0) + 0.3 * (std::sin(x[i - 1]) - s1);
        }
    }
    [[nodiscard]] crd::usize num_residuals() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] bool jacobian(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> jac) const override
    {
        for (crd::usize k = 0; k < m_n * m_n; ++k)
        {
            jac[k] = 0.0;
        }
        jac[0] = 1.0; // ∂r0/∂x0
        for (crd::usize i = 1; i < m_n; ++i)
        {
            jac[i * m_n + (i - 1)] = 0.3 * std::cos(x[i - 1]); // ∂r_i/∂x_{i-1}
            jac[i * m_n + i] = 1.0;                            // ∂r_i/∂x_i
        }
        return true;
    }
    [[nodiscard]] bool sparse_jacobian(crd::containers::ConstSpan<crd::f64> x,
                                       sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& out) const override
    {
        sp::TripletBuilder<crd::f64> tb(m_alloc, static_cast<crd::u32>(m_n), static_cast<crd::u32>(m_n));
        tb.add(0, 0, 1.0);
        for (crd::u32 i = 1; i < static_cast<crd::u32>(m_n); ++i)
        {
            tb.add(i, i - 1, 0.3 * std::cos(x[i - 1]));
            tb.add(i, i, 1.0);
        }
        out = tb.compress();
        return true;
    }

private:
    crd::usize               m_n;
    crd::memory::IAllocator*  m_alloc;
};
} // namespace

TEST_CASE("v7-e-2 sparse-LM converges on a nonlinear sparse chain", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 16;
    SparseChain res(n, &alloc);

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x0[i] = 0.9; // in the all-ones basin
    }
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;
    auto r = opt::minimize_levenberg_marquardt_sparse<crd::f64>(res, {x0.data(), n}, opts, &alloc);

    REQUIRE(r.status == opt::OptStatus::Success);
    crd::f64 err = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        err = std::max(err, std::fabs(r.x[i] - 1.0));
    }
    CHECK(err < 1e-6);
    CHECK(r.fx < 1e-12);
}

TEST_CASE("v7-e-2 sparse-LM lands at the same minimizer as dense LM", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::usize n = 12;
    SparseChain res(n, &alloc);

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x0[i] = 0.7;
    }
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-8;
    opts.max_iters = 200;
    auto rs = opt::minimize_levenberg_marquardt_sparse<crd::f64>(res, {x0.data(), n}, opts, &alloc);
    auto rd = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), n}, opts, &alloc);

    REQUIRE(rs.status == opt::OptStatus::Success);
    REQUIRE(rd.status == opt::OptStatus::Success);
    crd::f64 diff = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        diff = std::max(diff, std::fabs(rs.x[i] - rd.x[i]));
    }
    CHECK(diff < 1e-8); // sparse path reaches the same minimizer as the dense path
}

TEST_CASE("v7-e-2 sparse-LM determinism moat {1,2,4,8,16} (supernodal factor)", "[hesap][opt][v7][moat]")
{
    const crd::usize n = 48;
    crd::memory::TlsfAllocator alloc(1U << 24);

    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    crd::usize iters_ref = 0;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            SparseChain res(n, &alloc);
            crd::containers::Array<crd::f64> x0(&alloc);
            x0.resize(n);
            for (crd::usize i = 0; i < n; ++i)
            {
                x0[i] = 0.6;
            }
            opt::OptOptions<crd::f64> opts;
            opts.grad_tol = 1e-8;
            opts.max_iters = 200;
            auto r = opt::minimize_levenberg_marquardt_sparse<crd::f64>(res, {x0.data(), n}, opts, &alloc, 1e-3, nw);

            REQUIRE(r.status == opt::OptStatus::Success);
            REQUIRE(r.iterations > 1); // multi-iteration ⇒ the moat isn't a one-step coincidence
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
                CHECK(ident); // sparse-LM trajectory bit-identical across worker counts (supernodal factor moat)
            }
        }
        crd::jobs::shutdown();
    }
}
