// crd-hesap-eigen v6-a — symmetric Lanczos: correctness vs the analytic 1D-Laplacian spectrum + determinism
// + the {1,2,4,8}-worker moat (bit-identical eigenpairs; well-SEPARATED spectrum so eigenvectors are unique).

#include <crd/containers/array.hpp>
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

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// 1D Laplacian (tridiagonal: diag 2, off-diag −1). Eigenvalues λ_k = 2 − 2·cos(kπ/(n+1)), k=1..n — REAL,
// well separated at the extremes (the largest/smallest are isolated ⇒ unique eigenvectors for the moat).
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

double lambda_k(crd::u32 n, crd::u32 k) // k = 1..n
{
    const double pi = 3.14159265358979323846;
    return 2.0 - 2.0 * std::cos(static_cast<double>(k) * pi / static_cast<double>(n + 1));
}
} // namespace

TEST_CASE("v6-a symmetric Lanczos recovers the largest/smallest Laplacian eigenvalues", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 16; // default ncv=20 clamps to n ⇒ FULL Lanczos ⇒ every eigenpair converges exactly
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    // Largest 4 (algebraic): λ_n, λ_{n-1}, λ_{n-2}, λ_{n-3}.
    {
        eig::EigenOptions<crd::f64> opts;
        opts.nev = 4;
        opts.which = eig::Which::LargestAlgebraic;
        auto r = eig::eigs_sym<crd::f64>(op, opts, &alloc);
        REQUIRE(r.values.size() == 4);
        REQUIRE(r.converged);
        for (crd::u32 s = 0; s < 4; ++s)
        {
            CHECK(std::fabs(r.values[s].re - lambda_k(n, n - s)) < 1e-9);
            CHECK(r.residuals[s] < 1e-9);
        }
    }
    // Smallest 4 (algebraic): λ_1, λ_2, λ_3, λ_4.
    {
        eig::EigenOptions<crd::f64> opts;
        opts.nev = 4;
        opts.which = eig::Which::SmallestAlgebraic;
        auto r = eig::eigs_sym<crd::f64>(op, opts, &alloc);
        REQUIRE(r.values.size() == 4);
        REQUIRE(r.converged);
        for (crd::u32 s = 0; s < 4; ++s)
        {
            CHECK(std::fabs(r.values[s].re - lambda_k(n, s + 1)) < 1e-9);
            CHECK(r.residuals[s] < 1e-9);
        }
    }
}

TEST_CASE("v6-a symmetric Lanczos is deterministic (run-twice bit-identical)", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 50;
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);
    eig::EigenOptions<crd::f64> opts;
    opts.nev = 3;
    opts.which = eig::Which::LargestAlgebraic;

    auto r1 = eig::eigs_sym<crd::f64>(op, opts, &alloc);
    auto r2 = eig::eigs_sym<crd::f64>(op, opts, &alloc);
    REQUIRE(r1.values.size() == r2.values.size());
    bool ident = true;
    for (crd::u32 s = 0; s < r1.values.size() && ident; ++s)
    {
        ident = (r1.values[s].re == r2.values[s].re);
    }
    for (crd::usize i = 0; i < r1.vectors.size() && ident; ++i)
    {
        ident = (r1.vectors[i] == r2.vectors[i]);
    }
    CHECK(ident);
}

TEST_CASE("v6-a symmetric Lanczos determinism moat {1,2,4,8} (parallel spmv)", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 200; // big enough that the forced-parallel spmv genuinely splits rows across workers
    Csr a = laplacian_1d(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::LargestAlgebraic;

    crd::containers::Array<crd::f64> val_ref(&alloc);
    crd::containers::Array<crd::f64> vec_ref(&alloc);
    bool have_ref = false;

    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0); // force parallel
            auto r = eig::eigs_sym<crd::f64>(op, opts, &alloc);
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
                    ident = (r.values[s].re == val_ref[s]); // bit-identical eigenvalues
                }
                for (crd::usize i = 0; i < r.vectors.size() && ident; ++i)
                {
                    ident = (r.vectors[i] == vec_ref[i]); // bit-identical eigenvectors
                }
                CHECK(ident);
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v6-b thick-restart Lanczos converges the CLUSTERED largest Laplacian eigenvalues at bounded ncv",
          "[hesap][eigen][v6]")
{
    // The v6-b headline: n=64 with default ncv=20 << n ⇒ RESTART required. The largest eigenvalues are
    // clustered near 4 (the v6-a no-restart pass could NOT converge them); thick-restart does.
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 64;
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::LargestAlgebraic;
    opts.tol = 1e-10;
    auto r = eig::eigs_sym_tr<crd::f64>(op, opts, &alloc);
    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged); // restart converges what the single no-restart pass could not
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(r.values[s].re - lambda_k(n, n - s)) < 1e-8);
        CHECK(r.residuals[s] < 1e-9);
    }
}

TEST_CASE("v6-b thick-restart Lanczos determinism moat {1,2,4,8}", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 n = 240; // multi-restart at ncv=30 ⇒ many spmv calls across the cycles
    Csr a = laplacian_1d(&alloc, n);
    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic; // well-separated end ⇒ unique eigenvectors (moat ground rule)
    opts.ncv = 30;
    opts.tol = 1e-10;

    crd::containers::Array<crd::f64> val_ref(&alloc);
    crd::containers::Array<crd::f64> vec_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::eigs_sym_tr<crd::f64>(op, opts, &alloc);
            REQUIRE(r.values.size() == 4);
            REQUIRE(r.converged);
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
                CHECK(ident);
            }
        }
        crd::jobs::shutdown();
    }
}
