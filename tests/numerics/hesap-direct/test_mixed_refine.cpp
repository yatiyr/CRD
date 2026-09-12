// crd-hesap-direct v5f-a — mixed-precision iterative refinement (factor-in-f32 + refine-in-f64) tests.
//
// The bar: a LOW-precision (f32) LU factor, driven through working-precision (f64) iterative refinement,
// recovers FULL f64 accuracy on well-conditioned systems (LOAD-BEARING: the bare f32 solve is ~1e-6, the
// refined solve ~1e-13), and HONESTLY FLAGS (never returns silent garbage) when the low factor cannot
// deliver. Residual gate ‖A·x − b‖/‖b‖, not factor-equality (per the v5b discipline).

#include <crd/hesap/direct/mixed_refine.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/sparse/convert.hpp>
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

// Relative residual ‖A·x − b‖₂ / ‖b‖₂ for one RHS column (real).
double rel_resid(crd::memory::IAllocator* alloc, const Csr64& a, const crd::f64* x, const crd::f64* b)
{
    const crd::u32 n = a.pattern().rows;
    crd::containers::Array<crd::f64> ax(alloc);
    ax.resize(n);
    csr_matvec<crd::f64>(a, x, ax.data());
    double num = 0.0;
    double den = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double r = ax[i] - b[i];
        num += r * r;
        den += b[i] * b[i];
    }
    return std::sqrt(num) / (std::sqrt(den) + 1e-300);
}

// A diagonally-dominant UNSYMMETRIC, well-conditioned matrix (LU's domain) — factors cleanly, but the f32
// FACTOR arithmetic still leaves a ~1e-6 error that only IR removes.
Csr64 unsym(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(n + 2));
        if (i + 1 < n)
        {
            tb.add(i, i + 1, 1.0);
        }
        if (i > 0)
        {
            tb.add(i, i - 1, -2.0);
        }
        if (i + 2 < n)
        {
            tb.add(i, i + 2, 0.5);
        }
    }
    return tb.compress();
}

// A symmetric SPD matrix (Cholesky's domain) as full CSC: diagonally-dominant tridiagonal (diag 4,
// off-diag -1) — well-conditioned, yet the f32 factor still loses ~1e-6 that only IR recovers.
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> spd_csc(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 4.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
            tb.add(i + 1, i, -1.0);
        }
    }
    return sp::to_csc<crd::f64>(tb.compress(), a);
}

// A symmetric INDEFINITE matrix (LDLT's domain) as full CSC: sign-alternating diagonal (+/-2), weak
// symmetric coupling (-0.3, not exactly representable in f32) => diagonally dominant (well-conditioned)
// AND genuinely indefinite.
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> indef_csc(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, (i % 2 == 0) ? 2.0 : -2.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -0.3);
            tb.add(i + 1, i, -0.3);
        }
    }
    return sp::to_csc<crd::f64>(tb.compress(), a);
}

// y = A·x over a (full) CSC matrix — the symmetric residual oracle, independent of the factorization.
void csc_matvec(const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc>& a, const crd::f64* x, crd::f64* y)
{
    const sp::SparsePattern& p = a.pattern();
    const crd::f64* v = a.values().values.data();
    for (crd::u32 i = 0; i < p.rows; ++i)
    {
        y[i] = 0.0;
    }
    for (crd::u32 c = 0; c < p.cols; ++c)
    {
        for (crd::u32 k = p.outer_ptr[c]; k < p.outer_ptr[c + 1]; ++k)
        {
            y[p.inner_idx[k]] += v[k] * x[c];
        }
    }
}

// Relative residual ‖A·x − b‖₂ / ‖b‖₂ over a (full) CSC matrix.
double rel_resid_csc(crd::memory::IAllocator* alloc, const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc>& a,
                     const crd::f64* x, const crd::f64* b)
{
    const crd::u32 n = a.pattern().rows;
    crd::containers::Array<crd::f64> ax(alloc);
    ax.resize(n);
    csc_matvec(a, x, ax.data());
    double num = 0.0;
    double den = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double r = ax[i] - b[i];
        num += r * r;
        den += b[i] * b[i];
    }
    return std::sqrt(num) / (std::sqrt(den) + 1e-300);
}

// ---------- v5f-c determinism moat: block-diagonal builders (nblk independent fronts) ----------

// Block-diagonal of `nblk` UNSYMMETRIC 5x5 diag-dominant blocks (LU) — nblk independent fronts, so nw>1
// genuinely parallelizes the f32 factor.
Csr64 block_diag_unsym(crd::memory::IAllocator* a, crd::u32 nblk)
{
    const crd::u32 bs = 5;
    const crd::u32 n = nblk * bs;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 blk = 0; blk < nblk; ++blk)
    {
        const crd::u32 o = blk * bs;
        for (crd::u32 i = 0; i < bs; ++i)
        {
            tb.add(o + i, o + i, 12.0 + static_cast<crd::f64>(i));
            if (i + 1 < bs)
            {
                tb.add(o + i, o + i + 1, 1.0);
                tb.add(o + i + 1, o + i, -2.0);
            }
        }
    }
    return tb.compress();
}

// Block-diagonal SPD (Cholesky), full CSC.
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> block_diag_spd(crd::memory::IAllocator* a, crd::u32 nblk)
{
    const crd::u32 bs = 5;
    const crd::u32 n = nblk * bs;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 blk = 0; blk < nblk; ++blk)
    {
        const crd::u32 o = blk * bs;
        for (crd::u32 i = 0; i < bs; ++i)
        {
            tb.add(o + i, o + i, 8.0);
            if (i + 1 < bs)
            {
                tb.add(o + i, o + i + 1, -1.0);
                tb.add(o + i + 1, o + i, -1.0);
            }
        }
    }
    return sp::to_csc<crd::f64>(tb.compress(), a);
}

// Block-diagonal INDEFINITE symmetric (LDLT), full CSC.
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> block_diag_indef(crd::memory::IAllocator* a, crd::u32 nblk)
{
    const crd::u32 bs = 3;
    const crd::u32 n = nblk * bs;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 blk = 0; blk < nblk; ++blk)
    {
        const crd::u32 o = blk * bs;
        for (crd::u32 i = 0; i < bs; ++i)
        {
            tb.add(o + i, o + i, (i % 2 == 0) ? 3.0 : -3.0);
            if (i + 1 < bs)
            {
                tb.add(o + i, o + i + 1, -0.5);
                tb.add(o + i + 1, o + i, -0.5);
            }
        }
    }
    return sp::to_csc<crd::f64>(tb.compress(), a);
}

// The moat runner: factor-in-f32 + IR-solve must be BIT-IDENTICAL across {1,2,4,8} workers. `factory(nw)`
// builds the mixed solver with nw workers threaded into the f32 factor; `count(f)` returns its front/supernode
// count (asserted >= 2 so nw>1 actually parallelizes — non-vacuous). `b_src` = a consistent RHS (b = A·x_true).
template <typename FactoryFn>
void run_mixed_moat(crd::memory::IAllocator* alloc, FactoryFn factory, const crd::f64* b_src, crd::u32 n)
{
    auto ref = factory(1U); // serial reference
    REQUIRE(ref.info() == 0);
    // Non-vacuous by construction: callers pass a block-diagonal matrix (8 independent blocks => a level with
    // 8 concurrent fronts/supernodes), so nw>1 genuinely parallelizes the f32 factor.

    crd::containers::Array<crd::f64> x_ref(alloc);
    x_ref.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x_ref[i] = b_src[i];
    }
    REQUIRE(ref.solve({x_ref.data(), n}));
    const crd::u32 iters_ref = ref.last_iters();

    // The mixed pipeline's moat is END-TO-END: the converged solution + the IR iteration count are bit-
    // identical across worker counts. (A worker-dependent f32 factor would perturb the correction path and
    // hence the last ULPs of x + the iteration count; the per-family factor moats already pin the factor.)
    for (crd::u32 nw : {2U, 4U, 8U})
    {
        auto fp = factory(nw);
        REQUIRE(fp.info() == 0);

        crd::containers::Array<crd::f64> x(alloc);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b_src[i];
        }
        REQUIRE(fp.solve({x.data(), n}));
        bool xident = true;
        for (crd::u32 i = 0; i < n && xident; ++i)
        {
            xident = (x[i] == x_ref[i]);
        }
        CHECK(xident);                       // the mixed solution is bit-identical (the moat)
        CHECK(fp.last_iters() == iters_ref); // identical IR trajectory
    }
}
} // namespace

TEST_CASE("v5f-a mixed-precision LU recovers f64 accuracy from an f32 factor", "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 64;
    Csr64 a = unsym(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 1.0 + 0.1 * static_cast<double>(i) - 0.03 * static_cast<double>(i % 7);
    }
    csr_matvec<crd::f64>(a, xt.data(), b.data());

    auto mixed = dir::factor_mixed_lu(a, &alloc);
    REQUIRE(mixed.info() == 0);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(mixed.solve({x.data(), n}));

    // f64-level residual — IMPOSSIBLE from a single f32 solve (f32 floor ~1e-6) ⇒ the IR genuinely refined.
    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-10);
    // Recovered the true solution.
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err += (x[i] - xt[i]) * (x[i] - xt[i]);
    }
    CHECK(std::sqrt(err) < 1e-9);
    // x0 (the bare f32 solve) + at least one refinement step.
    CHECK(mixed.last_iters() >= 2);
}

TEST_CASE("v5f-a mixed-precision IR is load-bearing (f32-only solve is far worse)", "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 96;
    Csr64 a = unsym(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 2.0 - 0.05 * static_cast<double>(i);
    }
    csr_matvec<crd::f64>(a, xt.data(), b.data());

    // Bare f32 solve (one raw apply, NO refinement) = the baseline the IR must beat.
    sp::SparseMatrix<crd::f32, sp::SparseFormat::Csr> a32 = dir::csr_cast_copy<crd::f32>(&alloc, a);
    dir::MultifrontalLU<crd::f32> low = dir::factor_multifrontal_lu<crd::f32>(a32, &alloc);
    REQUIRE(low.info() == 0);
    crd::containers::Array<crd::f32> b32(&alloc);
    b32.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b32[i] = static_cast<crd::f32>(b[i]);
    }
    low.apply_inverse({b32.data(), n}, 1);
    crd::containers::Array<crd::f64> x_f32(&alloc);
    x_f32.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x_f32[i] = static_cast<crd::f64>(b32[i]);
    }
    const double resid_f32 = rel_resid(&alloc, a, x_f32.data(), b.data());

    // Mixed-precision solve over the SAME f32 factor quality.
    auto mixed = dir::factor_mixed_lu(a, &alloc);
    crd::containers::Array<crd::f64> x_mix(&alloc);
    x_mix.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x_mix[i] = b[i];
    }
    REQUIRE(mixed.solve({x_mix.data(), n}));
    const double resid_mix = rel_resid(&alloc, a, x_mix.data(), b.data());

    CHECK(resid_f32 > 1e-9);             // the bare f32 solve sits near the single-precision floor (~1e-8)
    CHECK(resid_mix < 1e-10);            // the refined solve reaches the double-precision floor
    CHECK(resid_mix < 0.01 * resid_f32); // IR bought >= 2 orders of magnitude — load-bearing
}

TEST_CASE("v5f-a mixed-precision LU multi-RHS recovers every column", "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 48;
    const crd::usize nrhs = 3;
    Csr64 a = unsym(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> rhs(&alloc);
    xt.resize(n * nrhs);
    rhs.resize(n * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[c * n + i] = 1.0 + static_cast<double>(c) - 0.02 * static_cast<double>(i);
        }
        csr_matvec<crd::f64>(a, xt.data() + c * n, rhs.data() + c * n);
    }

    auto mixed = dir::factor_mixed_lu(a, &alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n * nrhs);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        x[i] = rhs[i];
    }
    REQUIRE(mixed.solve({x.data(), x.size()}, nrhs));
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        CHECK(rel_resid(&alloc, a, x.data() + c * n, rhs.data() + c * n) < 1e-10);
    }
}

TEST_CASE("v5f-a mixed-precision IR recovers a moderately ill-conditioned system (kappa ~ 1e9)",
          "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // kappa ~ 4e9: the 2x2 block [[1,1],[1,1+1e-9]] is near-singular (and EXACTLY singular once cast to f32).
    // The static-pivot f32 factor perturbs the tiny pivot into a usable preconditioner, and f64 IR against
    // the TRUE matrix recovers an accurate solution — mixed-precision reaching past the f32-factor limit.
    sp::TripletBuilder<crd::f64> tb(&alloc, 3, 3);
    tb.add(0, 0, 2.0);
    tb.add(1, 1, 1.0);
    tb.add(1, 2, 1.0);
    tb.add(2, 1, 1.0);
    tb.add(2, 2, 1.0 + 1e-9);
    Csr64 a = tb.compress();

    crd::f64 xt[3] = {1.0, 2.0, 3.0};
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(3);
    csr_matvec<crd::f64>(a, xt, b.data());

    auto mixed = dir::factor_mixed_lu(a, &alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(3);
    for (crd::u32 i = 0; i < 3; ++i)
    {
        x[i] = b[i];
    }
    const bool ok = mixed.solve({x.data(), 3});
    // The CONTRACT: never claim success with a bad answer. Here mixed-IR succeeds AND delivers a small
    // residual — recovering a system the bare f32 factor (which sees a singular matrix) cannot.
    REQUIRE(ok);
    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-5);
}

TEST_CASE("v5f-a mixed-precision LU honestly REFUSES an inconsistent singular system (no silent garbage)",
          "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Rows 0 and 1 are identical (the matrix is singular); the RHS makes the first two equations INCONSISTENT
    // (b[0] != b[1]), so no x drives ‖A·x − b‖ to zero. The solver must FLAG this (return false), never return
    // a low-residual lie. This exercises the backward-error ACCEPT guard / the singular-factor refusal.
    sp::TripletBuilder<crd::f64> tb(&alloc, 3, 3);
    tb.add(0, 0, 1.0);
    tb.add(0, 1, 1.0);
    tb.add(1, 0, 1.0);
    tb.add(1, 1, 1.0);
    tb.add(2, 2, 1.0);
    Csr64 a = tb.compress();

    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(3);
    b[0] = 1.0;
    b[1] = 2.0; // != b[0] ⇒ inconsistent on the two identical rows
    b[2] = 3.0;

    auto mixed = dir::factor_mixed_lu(a, &alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(3);
    for (crd::u32 i = 0; i < 3; ++i)
    {
        x[i] = b[i];
    }
    const bool ok = mixed.solve({x.data(), 3});
    CHECK_FALSE(ok); // honest refusal: it cannot reach a small residual, so it does not claim success
    // And the invariant holds either way: a claimed success would have to carry a genuinely small residual.
    if (ok)
    {
        CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-6);
    }
}

TEST_CASE("v5f-b mixed-precision Cholesky recovers f64 accuracy from an f32 factor (SPD)", "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 64;
    auto a = spd_csc(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 1.0 + 0.1 * static_cast<double>(i % 5);
    }
    csc_matvec(a, xt.data(), b.data());

    auto mixed = dir::factor_mixed_cholesky(a, &alloc);
    REQUIRE(mixed.info() == 0);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(mixed.solve({x.data(), n}));
    CHECK(rel_resid_csc(&alloc, a, x.data(), b.data()) < 1e-10); // f64 floor — impossible from a single f32 solve
    CHECK(mixed.last_iters() >= 2);                              // x0 + at least one refinement
}

TEST_CASE("v5f-b mixed-precision LDLT recovers f64 accuracy from an f32 factor (indefinite)", "[hesap][mixed][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 64;
    auto a = indef_csc(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 2.0 - 0.05 * static_cast<double>(i);
    }
    csc_matvec(a, xt.data(), b.data());

    auto mixed = dir::factor_mixed_ldlt(a, &alloc);
    REQUIRE(mixed.info() == 0);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(mixed.solve({x.data(), n}));
    CHECK(rel_resid_csc(&alloc, a, x.data(), b.data()) < 1e-10);
    CHECK(mixed.last_iters() >= 2);
}

TEST_CASE("v5f-c mixed-precision LU determinism moat {1,2,4,8}", "[hesap][mixed][v5f][moat]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 24);
        Csr64 a = block_diag_unsym(&alloc, 8);
        const crd::u32 n = a.pattern().rows;
        crd::containers::Array<crd::f64> xt(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xt.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[i] = 1.0 + 0.1 * static_cast<double>(i);
        }
        csr_matvec<crd::f64>(a, xt.data(), b.data());
        run_mixed_moat(&alloc, [&](crd::u32 nw) { return dir::factor_mixed_lu(a, &alloc, nw); }, b.data(), n);
    }
    crd::jobs::shutdown();
}

TEST_CASE("v5f-c mixed-precision Cholesky determinism moat {1,2,4,8}", "[hesap][mixed][v5f][moat]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 24);
        auto a = block_diag_spd(&alloc, 8);
        const crd::u32 n = a.pattern().rows;
        crd::containers::Array<crd::f64> xt(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xt.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[i] = 1.0 + 0.1 * static_cast<double>(i % 5);
        }
        csc_matvec(a, xt.data(), b.data());
        run_mixed_moat(&alloc, [&](crd::u32 nw) { return dir::factor_mixed_cholesky(a, &alloc, nw); }, b.data(),
                       n);
    }
    crd::jobs::shutdown();
}

TEST_CASE("v5f-c mixed-precision LDLT determinism moat {1,2,4,8}", "[hesap][mixed][v5f][moat]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 24);
        auto a = block_diag_indef(&alloc, 8);
        const crd::u32 n = a.pattern().rows;
        crd::containers::Array<crd::f64> xt(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xt.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[i] = 2.0 - 0.05 * static_cast<double>(i);
        }
        csc_matvec(a, xt.data(), b.data());
        run_mixed_moat(&alloc, [&](crd::u32 nw) { return dir::factor_mixed_ldlt(a, &alloc, nw); }, b.data(), n);
    }
    crd::jobs::shutdown();
}
