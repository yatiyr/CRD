// crd-hesap-direct v5b-1 — Gilbert-Peierls sparse LU (serial correctness oracle) tests.
//
// The bar (advisor-pinned): gate on the RESIDUAL ‖A·x − b‖/‖b‖ and on solving a
// known x_true — NOT on factor-equality (different pivot choices give different,
// equally valid L/U; comparing factors would fail spuriously). Determinism here is
// run-to-run (a fixed matrix is a pure function); it is explicitly NOT the
// {1,2,4,8}-worker moat — v5b-1 is serial by construction (no workers exist). The
// deterministic + parallel LU is v5b-2 (MC64 + threshold static pivot).

#include <crd/hesap/complex.hpp>
#include <crd/hesap/direct/sparse_lu.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr64 = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;
using CCsr = sp::SparseMatrix<crd::hesap::Complex64, sp::SparseFormat::Csr>;

// y = A·x over a CSR matrix (the residual oracle — independent of the factorization).
template <typename T> void csr_matvec(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, const T* x, T* y)
{
    const sp::SparsePattern& p = a.pattern();
    const T* v = a.values().values.data();
    for (crd::u32 r = 0; r < p.rows; ++r)
    {
        T acc{};
        for (crd::u32 k = p.outer_ptr[r]; k < p.outer_ptr[r + 1]; ++k)
        {
            acc = acc + v[k] * x[p.inner_idx[k]];
        }
        y[r] = acc;
    }
}

// Relative residual ‖A·x − b‖₂ / ‖b‖₂ (real).
double rel_resid(crd::memory::IAllocator* alloc, const Csr64& a, const crd::f64* x, const crd::f64* b)
{
    const crd::u32 n = a.pattern().rows;
    double num = 0.0;
    double den = 0.0;
    crd::containers::Array<crd::f64> ax(alloc);
    ax.resize(n);
    csr_matvec<crd::f64>(a, x, ax.data());
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double r = ax[i] - b[i];
        num += r * r;
        den += b[i] * b[i];
    }
    return std::sqrt(num) / (std::sqrt(den) + 1e-300);
}

// A diagonally-dominant UNSYMMETRIC matrix (LU's domain): diagonal n+2, plus a few
// asymmetric off-diagonal entries (upper ≠ lower) — factors cleanly without pivoting.
Csr64 unsym(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(n + 2));
        if (i + 1 < n)
        {
            tb.add(i, i + 1, 1.0); // upper
        }
        if (i > 0)
        {
            tb.add(i, i - 1, -2.0); // lower (≠ upper ⇒ unsymmetric)
        }
        if (i + 2 < n)
        {
            tb.add(i, i + 2, 0.5);
        }
    }
    return tb.compress();
}
} // namespace

TEST_CASE("GP-LU solves a known x_true on an unsymmetric matrix (residual)", "[lu][v5b-1]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 60;
    Csr64 a = unsym(&alloc, n);
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);

    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    REQUIRE(lu.info() == 0);
    REQUIRE(lu.n() == n);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(lu.solve({x.data(), n}));

    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::abs(x[i] - xtrue[i]));
    }
    CHECK(err < 1e-10);
    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-12);
}

TEST_CASE("GP-LU 2D-grid Laplacian (residual)", "[lu][v5b-1]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 w = 12;
    const crd::u32 nn = w * w;
    sp::TripletBuilder<crd::f64> tb(&alloc, nn, nn);
    auto idx = [](crd::u32 r, crd::u32 c)
    {
        return r * w + c;
    }; // w is a constant expression — no capture
    for (crd::u32 r = 0; r < w; ++r)
    {
        for (crd::u32 c = 0; c < w; ++c)
        {
            const crd::u32 v = idx(r, c);
            tb.add(v, v, 4.0);
            if (r > 0)
                tb.add(v, idx(r - 1, c), -1.0);
            if (r + 1 < w)
                tb.add(v, idx(r + 1, c), -1.0);
            if (c > 0)
                tb.add(v, idx(r, c - 1), -1.0);
            if (c + 1 < w)
                tb.add(v, idx(r, c + 1), -1.0);
        }
    }
    Csr64 a = tb.compress();
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);
    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    REQUIRE(lu.info() == 0);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(nn);
    b.resize(nn);
    for (crd::u32 i = 0; i < nn; ++i)
    {
        xtrue[i] = std::sin(0.3 * static_cast<crd::f64>(i));
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(nn);
    for (crd::u32 i = 0; i < nn; ++i)
        x[i] = b[i];
    REQUIRE(lu.solve({x.data(), nn}));
    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-10);
}

TEST_CASE("GP-LU pivots: a zero diagonal forces a row interchange", "[lu][v5b-1]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // A = [[0,1,0],[1,0,1],[0,1,2]] — A(0,0)=0 ⇒ column 0 MUST pivot to row 1.
    sp::TripletBuilder<crd::f64> tb(&alloc, 3, 3);
    tb.add(0, 1, 1.0);
    tb.add(1, 0, 1.0);
    tb.add(1, 2, 1.0);
    tb.add(2, 1, 1.0);
    tb.add(2, 2, 2.0);
    Csr64 a = tb.compress();
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);
    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    REQUIRE(lu.info() == 0); // pivoting must make it nonsingular

    const crd::f64 xtrue[3] = {1.0, -2.0, 3.0};
    crd::f64 b[3];
    csr_matvec<crd::f64>(a, xtrue, b);
    crd::f64 x[3] = {b[0], b[1], b[2]};
    REQUIRE(lu.solve({x, 3}));
    for (crd::u32 i = 0; i < 3; ++i)
    {
        CHECK(std::abs(x[i] - xtrue[i]) < 1e-12);
    }
}

TEST_CASE("GP-LU detects a singular matrix (info != 0)", "[lu][v5b-1]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // Rank-deficient: column 2 is identically zero ⇒ no pivot at the last step.
    sp::TripletBuilder<crd::f64> tb(&alloc, 3, 3);
    tb.add(0, 0, 2.0);
    tb.add(1, 1, 3.0);
    tb.add(0, 1, 1.0);
    tb.add(2, 0, 1.0); // row 2 references col 0 only; col 2 empty ⇒ singular
    Csr64 a = tb.compress();
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);
    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    CHECK(lu.info() != 0);
    crd::f64 x[3] = {1.0, 1.0, 1.0};
    CHECK_FALSE(lu.solve({x, 3})); // solve refuses on a failed factor
}

TEST_CASE("GP-LU multi-RHS (3 columns at once)", "[lu][v5b-1]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 40;
    Csr64 a = unsym(&alloc, n);
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);
    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    REQUIRE(lu.info() == 0);

    const crd::usize nrhs = 3;
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> rhs(&alloc);
    xtrue.resize(static_cast<crd::usize>(n) * nrhs);
    rhs.resize(xtrue.size());
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[c * n + i] = static_cast<crd::f64>(1 + c) * (1.0 + 0.05 * static_cast<crd::f64>(i));
        }
        csr_matvec<crd::f64>(a, xtrue.data() + c * n, rhs.data() + c * n);
    }
    REQUIRE(lu.solve({rhs.data(), rhs.size()}, nrhs));
    double err = 0.0;
    for (crd::usize k = 0; k < rhs.size(); ++k)
    {
        err = std::max(err, std::abs(rhs[k] - xtrue[k]));
    }
    CHECK(err < 1e-10);
}

TEST_CASE("GP-LU complex (Complex64) residual", "[lu][v5b-1][complex]")
{
    using C = crd::hesap::Complex64;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 30;
    sp::TripletBuilder<C> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, C{static_cast<crd::f64>(n + 2), 1.0});
        if (i + 1 < n)
            tb.add(i, i + 1, C{1.0, -0.5});
        if (i > 0)
            tb.add(i, i - 1, C{-2.0, 0.3});
    }
    auto a = tb.compress();
    auto acsc = sp::to_csc<C>(a, &alloc);
    auto lu = dir::factor_gp_lu<C>(acsc, &alloc);
    REQUIRE(lu.info() == 0);

    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.1 * static_cast<crd::f64>(i), 0.2 * static_cast<crd::f64>(i)};
    }
    csr_matvec<C>(a, xtrue.data(), b.data());
    crd::containers::Array<C> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
        x[i] = b[i];
    REQUIRE(lu.solve({x.data(), n}));
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, crd::hesap::abs(x[i] - xtrue[i]));
    }
    CHECK(err < 1e-10);
}

TEST_CASE("GP-LU is run-to-run deterministic (a fixed matrix is a pure function)", "[lu][v5b-1]")
{
    // NOTE: this is run-to-run determinism, NOT the {1,2,4,8}-worker moat — v5b-1 is
    // serial (no workers). The deterministic PARALLEL LU is v5b-2 (MC64 + threshold).
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 50;
    Csr64 a = unsym(&alloc, n);
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);

    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
        b[i] = 1.0 + static_cast<crd::f64>(i % 7);

    crd::containers::Array<crd::f64> x1(&alloc);
    crd::containers::Array<crd::f64> x2(&alloc);
    x1.resize(n);
    x2.resize(n);
    auto lu1 = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    auto lu2 = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    REQUIRE(lu1.info() == 0);
    REQUIRE(lu2.info() == 0);
    REQUIRE(lu1.factor_nnz() == lu2.factor_nnz());
    for (crd::u32 i = 0; i < n; ++i)
    {
        x1[i] = b[i];
        x2[i] = b[i];
    }
    REQUIRE(lu1.solve({x1.data(), n}));
    REQUIRE(lu2.solve({x2.data(), n}));
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(x1[i] == x2[i]); // bit-identical
    }
}

TEST_CASE("GP-LU heavy pivoting at scale (row-permuted diagonally-dominant)", "[lu][v5b-1]")
{
    // A(i,:) = B(n-1-i,:), B row-diag-dominant (B(r,r)=2n, else 1). A's diagonal is thus 1
    // (non-dominant) and column j's max sits at row n-1-j ⇒ GP must pivot (a reverse permutation)
    // on EVERY column — exercises pinv≠identity, the final Li remap, and the x[pinv] solve perm at
    // scale. A is a row-permutation of a well-conditioned matrix ⇒ same singular values ⇒ tight residual.
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 24;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 r = n - 1 - i;
        for (crd::u32 j = 0; j < n; ++j)
        {
            tb.add(i, j, (r == j) ? static_cast<crd::f64>(2 * n) : 1.0);
        }
    }
    Csr64 a = tb.compress();
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);
    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    REQUIRE(lu.info() == 0); // pivoting must recover the well-conditioned system

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 - 0.05 * static_cast<crd::f64>(i);
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(lu.solve({x.data(), n}));
    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-10);
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::abs(x[i] - xtrue[i]));
    }
    CHECK(err < 1e-9);
}

TEST_CASE("GP-LU detects a numerically-zero pivot (a_max<=0 path)", "[lu][v5b-1]")
{
    // [[1,1],[1,1]]: after eliminating column 0 (pivot value 1), column 1's remaining entry
    // becomes 1 − 1·1 = 0 ⇒ no positive pivot candidate ⇒ the a_max<=0 failure path (distinct
    // from the empty-column path above, which fails at "no candidate at all").
    crd::memory::TlsfAllocator alloc(1U << 20);
    sp::TripletBuilder<crd::f64> tb(&alloc, 2, 2);
    tb.add(0, 0, 1.0);
    tb.add(0, 1, 1.0);
    tb.add(1, 0, 1.0);
    tb.add(1, 1, 1.0);
    Csr64 a = tb.compress();
    auto acsc = sp::to_csc<crd::f64>(a, &alloc);
    auto lu = dir::factor_gp_lu<crd::f64>(acsc, &alloc);
    CHECK(lu.info() != 0);
}
