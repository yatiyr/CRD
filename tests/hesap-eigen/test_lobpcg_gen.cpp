// crd-hesap-eigen v6-e-c — GENERALIZED symmetric LOBPCG A·x = λ·B·x (B SPD), the FEM K·x = λ·M·x form.
// Validates: (1) DIAGONAL B vs a dense reference (eig_sym of D^{-1/2}·A·D^{-1/2}, whose eigenvalues ARE the
// generalized eigenvalues) — rigorous smallest eigenvalues + exercises the B-image; (2) a NON-diagonal mass B —
// the generalized residual ‖A·x − λ·B·x‖ + B-orthonormality xᵀ·B·x = I; (3) the {1,2,4,8} determinism moat.

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

namespace eig = crd::hesap::eigen;
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

double diag_b(crd::u32 i) // a non-uniform SPD diagonal in [1, 2)
{
    return 1.0 + static_cast<double>((i * 37U + 11U) % 100U) / 100.0;
}

Csr diag_spd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, diag_b(i));
    }
    return tb.compress();
}

// Mass-like tridiagonal SPD B = tridiag(1, 4, 1) (strictly diagonally dominant ⇒ SPD).
Csr mass_tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 4.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, 1.0);
            tb.add(i + 1, i, 1.0);
        }
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v6-e-c generalized LOBPCG (diagonal B) matches the dense reference", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 24;
    Csr a = laplacian_1d(&alloc, n);
    Csr bd = diag_spd(&alloc, n);

    // Reference: C = D^{-1/2}·A·D^{-1/2} (symmetric tridiagonal); eig(C) = the generalized eigenvalues of (A, B).
    dn::Symmetric<crd::f64> c(&alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        c.at(i, i) = 2.0 / diag_b(i);
        if (i + 1 < n)
        {
            c.at(i + 1, i) = -1.0 / std::sqrt(diag_b(i) * diag_b(i + 1));
        }
    }
    dn::EigSym<crd::f64> ref = dn::eig_sym<crd::f64>(&alloc, c);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-9;
    opts.max_restarts = 500;
    sp::SparseLinearOp<crd::f64> aop(a);
    sp::SparseLinearOp<crd::f64> bop(bd);
    auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(aop, bop, opts, &alloc);
    REQUIRE(r.values.size() == 4);

    double got[4];
    for (crd::u32 s = 0; s < 4; ++s)
    {
        got[s] = r.values[s].re;
    }
    std::sort(got, got + 4);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(got[s] - ref.values.data()[s]) < 1e-7); // smallest 4 generalized eigenvalues
    }
    CHECK(r.converged);
}

TEST_CASE("v6-e-c generalized LOBPCG (mass B): residual + B-orthonormality", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 80;
    Csr a = laplacian_1d(&alloc, n);
    Csr bm = mass_tridiag(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-9;
    opts.max_restarts = 500;
    sp::SparseLinearOp<crd::f64> aop(a);
    sp::SparseLinearOp<crd::f64> bop(bm);
    auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(aop, bop, opts, &alloc);
    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged);

    crd::containers::Array<crd::f64> ax(&alloc);
    crd::containers::Array<crd::f64> bxj(&alloc);
    ax.resize(n);
    bxj.resize(n);
    for (crd::u32 j = 0; j < 4; ++j)
    {
        const crd::f64* xj = r.vectors.data() + static_cast<crd::usize>(j) * n;
        (void)aop.apply({xj, n}, {ax.data(), n});
        (void)bop.apply({xj, n}, {bxj.data(), n});
        crd::f64 rn2 = 0.0;
        crd::f64 an2 = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::f64 e = ax[i] - r.values[j].re * bxj[i];
            rn2 += e * e;
            an2 += ax[i] * ax[i];
        }
        CHECK(std::sqrt(rn2) / std::sqrt(an2) < 1e-6); // ‖A·x − λ·B·x‖ / ‖A·x‖
        // B-orthonormality: x_i^T·B·x_j = δ_ij.
        for (crd::u32 i = 0; i < 4; ++i)
        {
            const crd::f64* xi = r.vectors.data() + static_cast<crd::usize>(i) * n;
            crd::f64 d = 0.0;
            for (crd::u32 t = 0; t < n; ++t)
            {
                d += xi[t] * bxj[t];
            }
            CHECK(std::fabs(d - (i == j ? 1.0 : 0.0)) < 1e-6);
        }
    }
}

TEST_CASE("v6-e-c generalized LOBPCG determinism moat {1,2,4,8}", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 96;
    Csr a = laplacian_1d(&alloc, n);
    Csr bm = mass_tridiag(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-9;
    opts.max_restarts = 500;

    crd::containers::Array<crd::f64> val_ref(&alloc);
    crd::containers::Array<crd::f64> vec_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> aop(a, &alloc, /*parallel_min_stored_bytes=*/0);
            sp::ParallelSparseLinearOp<crd::f64> bop(bm, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(aop, bop, opts, &alloc);
            REQUIRE(r.values.size() == 4);
            if (!have_ref)
            {
                val_ref.resize(r.values.size());
                for (crd::u32 s = 0; s < r.values.size(); ++s)
                {
                    val_ref[s] = r.values[s].re;
                }
                vec_ref.resize(r.vectors.size());
                for (crd::usize i = 0; i < r.vectors.size(); ++i)
                {
                    vec_ref[i] = r.vectors[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (crd::u32 s = 0; s < r.values.size() && ident; ++s)
                {
                    ident = (r.values[s].re == val_ref[s]);
                }
                for (crd::usize i = 0; i < r.vectors.size() && ident; ++i)
                {
                    ident = (r.vectors[i] == vec_ref[i]);
                }
                CHECK(ident); // generalized eigenpairs bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
