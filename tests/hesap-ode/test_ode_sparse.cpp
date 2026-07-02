// v9-j gates: the sparse-Jacobian path (BDF + SparseOdeLinearSolver over the multifrontal LU) on
// method-of-lines heat diffusion — gated against the EXACT discrete-eigenmode solution (the discrete
// Laplacian's (1,1) eigenmode decays as e^{lambda_h t} exactly in the ODE system), the sparse-vs-dense
// cross-check at moderate n, and determinism.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/ode_sparse_solver.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f64;
using crd::u32;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;
namespace sp = crd::hesap::sparse;

namespace
{

// 2D heat equation MOL: u' = L_h u on the unit square, Dirichlet 0, m x m interior grid, spacing
// dx = 1/(m+1). Provides BOTH the dense and the sparse Jacobian so one class serves both paths.
class Heat2d final : public ode::OdeFunction<f64>
{
public:
    Heat2d(u32 m, bool sparse_mode, crd::memory::IAllocator* alloc)
        : ode::OdeFunction<f64>(/*dense jac*/ !sparse_mode), m_m(m), m_alloc(alloc)
    {
        set_has_sparse_jacobian(sparse_mode);
        m_inv_dx2 = static_cast<f64>((m + 1)) * static_cast<f64>((m + 1));
    }

    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        const u32 m = m_m;
        for (u32 i = 0; i < m; ++i)
        {
            for (u32 j = 0; j < m; ++j)
            {
                const usize k = static_cast<usize>(i) * m + j;
                const f64 up = (i > 0) ? y[k - m] : 0.0;
                const f64 down = (i + 1 < m) ? y[k + m] : 0.0;
                const f64 left = (j > 0) ? y[k - 1] : 0.0;
                const f64 right = (j + 1 < m) ? y[k + 1] : 0.0;
                d[k] = m_inv_dx2 * (up + down + left + right - 4.0 * y[k]);
            }
        }
    }

    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> jac) const override
    {
        const u32 m = m_m;
        const usize n = static_cast<usize>(m) * m;
        for (usize i = 0; i < n * n; ++i)
        {
            jac[i] = 0.0;
        }
        for (u32 i = 0; i < m; ++i)
        {
            for (u32 j = 0; j < m; ++j)
            {
                const usize k = static_cast<usize>(i) * m + j;
                jac[k * n + k] = -4.0 * m_inv_dx2;
                if (i > 0)
                {
                    jac[k * n + (k - m)] = m_inv_dx2;
                }
                if (i + 1 < m)
                {
                    jac[k * n + (k + m)] = m_inv_dx2;
                }
                if (j > 0)
                {
                    jac[k * n + (k - 1)] = m_inv_dx2;
                }
                if (j + 1 < m)
                {
                    jac[k * n + (k + 1)] = m_inv_dx2;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool sparse_jacobian(f64, containers::ConstSpan<f64>,
                                       sp::SparseMatrix<f64, sp::SparseFormat::Csr>& out) const override
    {
        const u32 m = m_m;
        const u32 n = m * m;
        sp::TripletBuilder<f64> tb(m_alloc, n, n);
        for (u32 i = 0; i < m; ++i)
        {
            for (u32 j = 0; j < m; ++j)
            {
                const u32 k = i * m + j;
                tb.add(k, k, -4.0 * m_inv_dx2);
                if (i > 0)
                {
                    tb.add(k, k - m, m_inv_dx2);
                }
                if (i + 1 < m)
                {
                    tb.add(k, k + m, m_inv_dx2);
                }
                if (j > 0)
                {
                    tb.add(k, k - 1, m_inv_dx2);
                }
                if (j + 1 < m)
                {
                    tb.add(k, k + 1, m_inv_dx2);
                }
            }
        }
        out = tb.compress();
        return true;
    }

    [[nodiscard]] usize dim() const noexcept override { return static_cast<usize>(m_m) * m_m; }

    // The (1,1) eigenmode of the DISCRETE Laplacian and its exact eigenvalue.
    void eigenmode(containers::Span<f64> y) const
    {
        const f64 dx = 1.0 / static_cast<f64>(m_m + 1);
        const f64 pi = 3.14159265358979323846;
        for (u32 i = 0; i < m_m; ++i)
        {
            for (u32 j = 0; j < m_m; ++j)
            {
                y[static_cast<usize>(i) * m_m + j] =
                    std::sin(pi * static_cast<f64>(i + 1) * dx) * std::sin(pi * static_cast<f64>(j + 1) * dx);
            }
        }
    }
    [[nodiscard]] f64 eigenvalue() const
    {
        const f64 dx = 1.0 / static_cast<f64>(m_m + 1);
        const f64 pi = 3.14159265358979323846;
        const f64 s = std::sin(pi * dx / 2.0);
        return -8.0 * m_inv_dx2 * s * s; // lambda_x + lambda_y = 2 * (-(4/dx^2) sin^2(pi dx / 2))
    }

private:
    u32 m_m;
    crd::memory::IAllocator* m_alloc;
    f64 m_inv_dx2;
};

} // namespace

TEST_CASE("sparse: heat-2D MOL eigenmode decays at the EXACT discrete rate (n = 1024)", "[ode][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const u32 m = 32; // n = 1024 — dense LU would be 1024^3 per refactor; sparse makes it routine
    Heat2d f(m, /*sparse*/ true, &alloc);
    ode::SparseOdeLinearSolver<f64> solver(&alloc);

    containers::Array<f64> y(&alloc);
    y.resize(f.dim());
    f.eigenmode(containers::Span<f64>(y.data(), y.size()));
    const f64 y0_mid = y[(static_cast<usize>(m / 2) * m) + m / 2];

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-12;
    const f64 t_end = 0.05;
    const ode::OdeResult<f64> r =
        ode::integrate_bdf<f64>(f, 0.0, t_end, containers::Span<f64>(y.data(), y.size()), opts, &alloc, &solver);
    REQUIRE(r.success);

    // u(t) = e^{lambda_h t} u0 EXACTLY for the ODE system (u0 = eigenvector of the discrete operator).
    const f64 decay = std::exp(f.eigenvalue() * t_end);
    const f64 mid = y[(static_cast<usize>(m / 2) * m) + m / 2];
    INFO("decay=" << decay << " naccept=" << r.work.naccept << " nlu=" << r.work.nlu << " nfev=" << r.work.nfev);
    CHECK(std::abs(mid - decay * y0_mid) < 1e-7);
    // And the whole field stays a scaled eigenmode (shape preserved): check a second point �
    // y[m + 1] is grid (i = 1, j = 1) ? sin(2p?)�sin(2p?).
    const f64 s2 = std::sin(2.0 * 3.14159265358979323846 / static_cast<f64>(m + 1));
    CHECK(std::abs(y[m + 1] - decay * s2 * s2) < 1e-7);
}

TEST_CASE("sparse: sparse path agrees with the dense path (n = 144 cross-check)", "[ode][sparse]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const u32 m = 12;
    Heat2d fs(m, true, &alloc);
    Heat2d fd(m, false, &alloc);
    ode::SparseOdeLinearSolver<f64> solver(&alloc);

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-9;
    opts.atol = 1e-12;

    containers::Array<f64> ys(&alloc);
    ys.resize(fs.dim());
    fs.eigenmode(containers::Span<f64>(ys.data(), ys.size()));
    const ode::OdeResult<f64> rs =
        ode::integrate_bdf<f64>(fs, 0.0, 0.05, containers::Span<f64>(ys.data(), ys.size()), opts, &alloc, &solver);
    REQUIRE(rs.success);

    containers::Array<f64> yd(&alloc);
    yd.resize(fd.dim());
    fd.eigenmode(containers::Span<f64>(yd.data(), yd.size()));
    const ode::OdeResult<f64> rd =
        ode::integrate_bdf<f64>(fd, 0.0, 0.05, containers::Span<f64>(yd.data(), yd.size()), opts, &alloc);
    REQUIRE(rd.success);

    // Same Jacobian values, same Newton decisions ⇒ identical work; states agree to solver roundoff.
    CHECK(rs.work.naccept == rd.work.naccept);
    CHECK(rs.work.nfev == rd.work.nfev);
    CHECK(rs.work.nlu == rd.work.nlu);
    f64 max_diff = 0.0;
    for (usize i = 0; i < ys.size(); ++i)
    {
        const f64 d = std::abs(ys[i] - yd[i]);
        max_diff = d > max_diff ? d : max_diff;
    }
    CHECK(max_diff < 1e-12);
}

TEST_CASE("sparse: run-twice bit identity at n = 1024", "[ode][sparse][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const u32 m = 32;
    Heat2d f(m, true, &alloc);

    auto run = [&](containers::Array<f64>& y) -> ode::OdeResult<f64>
    {
        ode::SparseOdeLinearSolver<f64> solver(&alloc);
        y.resize(f.dim());
        f.eigenmode(containers::Span<f64>(y.data(), y.size()));
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-7;
        opts.atol = 1e-10;
        return ode::integrate_bdf<f64>(f, 0.0, 0.05, containers::Span<f64>(y.data(), y.size()), opts, &alloc,
                                       &solver);
    };
    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    const ode::OdeResult<f64> r1 = run(a);
    const ode::OdeResult<f64> r2 = run(b);
    REQUIRE(r1.success);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.nlu == r2.work.nlu);
}
