// crd-hesap-direct v5f-d — mixed-precision LEAST-SQUARES iterative refinement (CSNE).
//
// The bar: an f32 multifrontal QR factor of an over-determined A (m≥n), driven through f64 corrected
// semi-normal-equations refinement, recovers FULL f64 LS accuracy (LOAD-BEARING: an f32-only LS solve floors
// at ~1e-6; the refined solve reaches ~1e-13), and the {1,2,4,8}-worker solution is bit-identical (the moat).
// Optimality gate = the NORMAL-equation residual ‖Aᵀ(b−A·x)‖, the LS condition (not a square A·x=b residual).

#include <crd/hesap/direct/mixed_qr_refine.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr64 = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// y(m) = A(m×n)·x(n) over a CSR matrix — the LS residual oracle, independent of the factorization.
void csr_matvec(const Csr64& a, const crd::f64* x, crd::f64* y)
{
    const sp::SparsePattern& p = a.pattern();
    const crd::f64* v = a.values().values.data();
    for (crd::u32 r = 0; r < p.rows; ++r)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 k = p.outer_ptr[r]; k < p.outer_ptr[r + 1]; ++k)
        {
            acc += v[k] * x[p.inner_idx[k]];
        }
        y[r] = acc;
    }
}

// A well-conditioned, full-column-rank, OVER-DETERMINED (m = 2n) sparse matrix: a tridiagonal top block
// stacked over an identity-plus-coupling bottom block (the identity block guarantees full column rank).
Csr64 overdet(crd::memory::IAllocator* a, crd::u32 n)
{
    const crd::u32 m = 2 * n;
    sp::TripletBuilder<crd::f64> tb(a, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 4.0); // top: tridiagonal
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
        }
        if (i > 0)
        {
            tb.add(i, i - 1, -1.0);
        }
        tb.add(n + i, i, 1.0);                 // bottom: identity (full column rank)
        tb.add(n + i, (i + 3) % n, 0.3);       // + weak coupling (not exactly representable in f32)
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v5f-d mixed-precision LS recovers f64 accuracy from an f32 QR factor", "[hesap][mixed-qr][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 64;
    const crd::u32 m = 2 * n;
    Csr64 a = overdet(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(m);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 1.0 + 0.1 * static_cast<double>(i) - 0.03 * static_cast<double>(i % 7);
    }
    csr_matvec(a, xt.data(), b.data()); // consistent ⇒ the LS solution is x_true (zero residual)

    auto qr = dir::factor_mixed_qr(a, &alloc);
    REQUIRE(qr.info() == 0);
    REQUIRE(qr.rows() == m);
    REQUIRE(qr.cols() == n);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    REQUIRE(qr.least_squares({b.data(), m}, {x.data(), n}, 1));

    // Recovered the true solution to f64 — IMPOSSIBLE from a single f32 LS solve (f32 floor ~1e-6).
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err += (x[i] - xt[i]) * (x[i] - xt[i]);
    }
    CHECK(std::sqrt(err) < 1e-10);
    CHECK(qr.last_iters() >= 2); // genuinely refined beyond the bare f32 solution
}

TEST_CASE("v5f-d mixed-precision LS drives the normal-equation residual to f64", "[hesap][mixed-qr][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 96;
    const crd::u32 m = 2 * n;
    Csr64 a = overdet(&alloc, n);

    // An INCONSISTENT RHS (b not in range(A)) — the LS solution has a nonzero residual; optimality is
    // ‖Aᵀ(b−A·x)‖ → 0, which is what CSNE refines (so this is the honest LS gate, not x recovery).
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        b[i] = std::sin(0.31 * static_cast<double>(i) + 0.2);
    }

    auto qr = dir::factor_mixed_qr(a, &alloc);
    REQUIRE(qr.info() == 0);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    REQUIRE(qr.least_squares({b.data(), m}, {x.data(), n}, 1));

    // Normal-equation residual ‖Aᵀ(b − A·x)‖∞ / ‖Aᵀb‖∞ — the LS optimality measure.
    crd::containers::Array<crd::f64> ax(&alloc);
    crd::containers::Array<crd::f64> r(&alloc);
    crd::containers::Array<crd::f64> atr(&alloc);
    crd::containers::Array<crd::f64> atb(&alloc);
    ax.resize(m);
    r.resize(m);
    atr.resize(n);
    atb.resize(n);
    sp::spmv<crd::f64>(1.0, a, sp::Trans::Transpose, {b.data(), m}, 0.0, {atb.data(), n});
    csr_matvec(a, x.data(), ax.data());
    for (crd::u32 i = 0; i < m; ++i)
    {
        r[i] = b[i] - ax[i];
    }
    sp::spmv<crd::f64>(1.0, a, sp::Trans::Transpose, {r.data(), m}, 0.0, {atr.data(), n});
    double rnorm = 0.0;
    double bnorm = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        rnorm = std::fabs(atr[i]) > rnorm ? std::fabs(atr[i]) : rnorm;
        bnorm = std::fabs(atb[i]) > bnorm ? std::fabs(atb[i]) : bnorm;
    }
    CHECK(rnorm / (bnorm + 1e-300) < 1e-9); // f64 LS optimality from an f32 factor
}

TEST_CASE("v5f-d mixed-precision LS is multi-RHS", "[hesap][mixed-qr][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 48;
    const crd::u32 m = 2 * n;
    const crd::usize nrhs = 3;
    Csr64 a = overdet(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n * nrhs);
    b.resize(m * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[c * n + i] = 0.5 + 0.2 * static_cast<double>(c) - 0.04 * static_cast<double>(i % 5);
        }
        csr_matvec(a, xt.data() + c * n, b.data() + c * m);
    }

    auto qr = dir::factor_mixed_qr(a, &alloc);
    REQUIRE(qr.info() == 0);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n * nrhs);
    REQUIRE(qr.least_squares({b.data(), m * nrhs}, {x.data(), n * nrhs}, nrhs));
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        double err = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            err += (x[c * n + i] - xt[c * n + i]) * (x[c * n + i] - xt[c * n + i]);
        }
        CHECK(std::sqrt(err) < 1e-10);
    }
}

TEST_CASE("v5f-d mixed-precision LS determinism moat {1,2,4,8}", "[hesap][mixed-qr][v5f][moat]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 24);
        const crd::u32 n = 80;
        const crd::u32 m = 2 * n;
        Csr64 a = overdet(&alloc, n);
        crd::containers::Array<crd::f64> xt(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xt.resize(n);
        b.resize(m);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[i] = 1.0 + 0.1 * static_cast<double>(i);
        }
        csr_matvec(a, xt.data(), b.data());

        auto factory = [&](crd::u32 nw) { return dir::factor_mixed_qr(a, &alloc, nw); };
        auto ref = factory(1U);
        REQUIRE(ref.info() == 0);
        crd::containers::Array<crd::f64> x_ref(&alloc);
        x_ref.resize(n);
        REQUIRE(ref.least_squares({b.data(), m}, {x_ref.data(), n}, 1));
        const crd::u32 iters_ref = ref.last_iters();

        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto fp = factory(nw);
            REQUIRE(fp.info() == 0);
            crd::containers::Array<crd::f64> x(&alloc);
            x.resize(n);
            REQUIRE(fp.least_squares({b.data(), m}, {x.data(), n}, 1));
            bool xident = true;
            for (crd::u32 i = 0; i < n && xident; ++i)
            {
                xident = (x[i] == x_ref[i]);
            }
            CHECK(xident);                       // bit-identical LS solution (the moat)
            CHECK(fp.last_iters() == iters_ref); // identical CSNE trajectory
        }
    }
    crd::jobs::shutdown();
}
