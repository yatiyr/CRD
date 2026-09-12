// crd-hesap-direct v5f — GMRES-based iterative refinement (Carson-Higham) over a sparse DIRECT factor.
//
// The bar: a static-pivot LU factor that is a POOR approximation of A (the saddle-point / indefinite
// unsymmetric regime where FIXED-POINT IR diverges) is rescued by FGMRES PRECONDITIONED by that same factor —
// the refined solve reaches f64 backward error AND the {1,2,4,8}-worker solution is bit-identical (the moat).
// The real-matrix divergence-FIX (garon2: fixed-point 2.9e-05 DIVERGED → GMRES-IR 3 iters 1.9e-15) is proven
// in bench_hesap_lu_supernodal_vs_reference; here we pin the API contract + correctness + the moat.

#include <crd/hesap/direct/gmres_refine.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
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

// Relative residual ‖A·x − b‖₂ / ‖b‖₂ for one RHS column.
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

// A well-conditioned diagonally-dominant UNSYMMETRIC matrix (the bare factor already solves this — the test
// is that GMRES-IR also reaches f64 and the API behaves).
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

// An indefinite, NOT diagonally-dominant UNSYMMETRIC matrix (sign-alternating diagonal + strong asymmetric
// coupling): the static-pivot factor is a weaker approximation here, exercising several GMRES iterations.
Csr64 indef_unsym(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, (i % 2 == 0) ? 1.5 : -1.5);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, 1.3);
            tb.add(i + 1, i, -0.7);
        }
        if (i + 3 < n)
        {
            tb.add(i, i + 3, 0.4);
        }
    }
    return tb.compress();
}

// Block-diagonal of `nblk` unsymmetric 5×5 diag-dominant blocks — nblk independent fronts ⇒ nw>1 genuinely
// parallelizes the factor (the moat is non-vacuous).
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
} // namespace

TEST_CASE("v5f GMRES-IR reaches f64 backward error on a well-conditioned unsymmetric system", "[hesap][gmres-ir][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 80;
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

    auto refined = dir::factor_gmres_refined_lu(a, &alloc);
    REQUIRE(refined.info() == 0);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    const bool ok = refined.solve({x.data(), n});
    CHECK(ok);
    CHECK(ok == refined.last_converged()); // the return value IS the convergence flag (honest contract)
    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-12);
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err += (x[i] - xt[i]) * (x[i] - xt[i]);
    }
    CHECK(std::sqrt(err) < 1e-10);
    CHECK(refined.last_iters() >= 1);
}

TEST_CASE("v5f GMRES-IR solves an indefinite system and never under-performs the bare factor", "[hesap][gmres-ir][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 120;
    Csr64 a = indef_unsym(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 0.5 + 0.2 * static_cast<double>(i % 11);
    }
    csr_matvec<crd::f64>(a, xt.data(), b.data());

    // Bare static-pivot factor (its own fixed-point IR).
    crd::containers::Array<crd::f64> xb(&alloc);
    xb.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xb[i] = b[i];
    }
    auto bare = dir::factor_multifrontal_lu<crd::f64>(a, &alloc);
    REQUIRE(bare.info() == 0);
    (void)bare.solve({xb.data(), n});
    const double bare_resid = rel_resid(&alloc, a, xb.data(), b.data());

    // GMRES-IR.
    auto refined = dir::factor_gmres_refined_lu(a, &alloc);
    REQUIRE(refined.info() == 0);
    crd::containers::Array<crd::f64> xg(&alloc);
    xg.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xg[i] = b[i];
    }
    REQUIRE(refined.solve({xg.data(), n}));
    const double gir_resid = rel_resid(&alloc, a, xg.data(), b.data());

    CHECK(gir_resid < 1e-11); // GMRES-IR reaches f64 backward error
    // Whenever the bare factor fails to reach f64 (the static-pivot divergence regime), GMRES-IR strictly
    // fixes it; when the bare factor already succeeds, GMRES-IR is at least as good.
    if (bare_resid > 1e-6)
    {
        CHECK(gir_resid < bare_resid);
    }
}

TEST_CASE("v5f GMRES-IR is multi-RHS (each column refined independently)", "[hesap][gmres-ir][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 64;
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
            xt[c * n + i] = 1.0 + 0.3 * static_cast<double>(c) - 0.05 * static_cast<double>(i % 5);
        }
        csr_matvec<crd::f64>(a, xt.data() + c * n, rhs.data() + c * n);
    }

    auto refined = dir::factor_gmres_refined_lu(a, &alloc);
    REQUIRE(refined.info() == 0);
    REQUIRE(refined.solve({rhs.data(), n * nrhs}, nrhs));
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        double err = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            err += (rhs[c * n + i] - xt[c * n + i]) * (rhs[c * n + i] - xt[c * n + i]);
        }
        CHECK(std::sqrt(err) < 1e-10); // each column recovered the true solution (rhs holds X on exit)
    }
}

TEST_CASE("v5f GMRES-IR determinism moat {1,2,4,8}", "[hesap][gmres-ir][v5f][moat]")
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

        auto factory = [&](crd::u32 nw) { return dir::factor_gmres_refined_lu(a, &alloc, nw); };
        auto ref = factory(1U);
        REQUIRE(ref.info() == 0);
        crd::containers::Array<crd::f64> x_ref(&alloc);
        x_ref.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x_ref[i] = b[i];
        }
        REQUIRE(ref.solve({x_ref.data(), n}));
        const crd::u32 iters_ref = ref.last_iters();

        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto fp = factory(nw);
            REQUIRE(fp.info() == 0);
            crd::containers::Array<crd::f64> x(&alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            REQUIRE(fp.solve({x.data(), n}));
            bool xident = true;
            for (crd::u32 i = 0; i < n && xident; ++i)
            {
                xident = (x[i] == x_ref[i]);
            }
            CHECK(xident);                      // the refined solution is bit-identical (the moat)
            CHECK(fp.last_iters() == iters_ref); // identical GMRES trajectory
        }
    }
    crd::jobs::shutdown();
}
