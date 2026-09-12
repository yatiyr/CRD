// crd-hesap-eigen v6-h — sparse SVD via Golub-Kahan-Lanczos bidiagonalization (IRLBA). Validates: (1) CORE —
// the largest singular triplets of a RECTANGULAR (tall, m>n) sparse A vs a dense `svd` oracle, with the
// COUPLED-sign correctness check A·v = σ·u AND Aᵀ·u = σ·v (catches an independent re-sign-pin or a U/V mix-up);
// (2) thick-restart converges at bounded ncv ≪ min(m,n); (3) the {1,2,4,8} determinism MOAT (both spmv
// directions forced parallel). ncv < min(m,n) throughout ⇒ genuinely iterative (not a trivially-exact full run).

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/svd.hpp>
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

// A tall rectangular A (m×n, n ≤ m ≤ 2n) with a SPIKED spectrum: the top 4 diagonal entries are geometrically
// separated (64, 32, 16, 8 — relative gaps of 2×, so no-restart GKL converges the top 4 in a few steps and the
// singular vectors are unique), the rest are a small distinct bulk (~2). + superdiagonal coupling + tall tail.
double a_entry_diag(crd::u32 i, crd::u32 /*n*/)
{
    if (i < 4)
    {
        return std::pow(2.0, 6.0 - static_cast<double>(i)); // 64, 32, 16, 8
    }
    return 2.0 - 0.02 * static_cast<double>(i); // small distinct bulk
}

Csr tall_sparse(crd::memory::IAllocator* a, crd::u32 m, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, a_entry_diag(i, n));
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
        }
    }
    for (crd::u32 i = n; i < m; ++i)
    {
        tb.add(i, i - n, 0.3); // tall-tail rows (i-n < n since m ≤ 2n)
    }
    return tb.compress();
}

dn::Matrix<crd::f64> tall_dense(crd::memory::IAllocator* a, crd::u32 m, crd::u32 n)
{
    dn::Matrix<crd::f64> mat(a, m, n); // zero-initialized
    for (crd::u32 i = 0; i < n; ++i)
    {
        mat.at(i, i) = a_entry_diag(i, n);
        if (i + 1 < n)
        {
            mat.at(i, i + 1) = -1.0;
        }
    }
    for (crd::u32 i = n; i < m; ++i)
    {
        mat.at(i, i - n) = 0.3;
    }
    return mat;
}

// A GRADED spectrum: the largest singular values are CLOSELY spaced (diagonal 2 + 0.5·(n−i), gaps ~0.5 over a
// spread ~20 ⇒ relative gaps ~0.02). No-restart GKL at bounded ncv canNOT converge the top few here — only the
// thick restart does (the v6-h-specific value, mirroring v6-a→v6-b for the symmetric case).
double a_graded_diag(crd::u32 i, crd::u32 n) { return 2.0 + 0.5 * static_cast<double>(n - i); }

Csr graded_sparse(crd::memory::IAllocator* a, crd::u32 m, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, a_graded_diag(i, n));
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
        }
    }
    for (crd::u32 i = n; i < m; ++i)
    {
        tb.add(i, i - n, 0.3);
    }
    return tb.compress();
}

dn::Matrix<crd::f64> graded_dense(crd::memory::IAllocator* a, crd::u32 m, crd::u32 n)
{
    dn::Matrix<crd::f64> mat(a, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        mat.at(i, i) = a_graded_diag(i, n);
        if (i + 1 < n)
        {
            mat.at(i, i + 1) = -1.0;
        }
    }
    for (crd::u32 i = n; i < m; ++i)
    {
        mat.at(i, i - n) = 0.3;
    }
    return mat;
}

// Check A·v_k = σ_k·u_k and Aᵀ·u_k = σ_k·v_k (the coupled-sign correctness), and unit norms.
void check_triplets(const eig::SvdResult<crd::f64>& r, const sp::SparseLinearOp<crd::f64>& op, crd::u32 cnt,
                    crd::memory::IAllocator* alloc)
{
    const crd::u32 m = r.m;
    const crd::u32 n = r.n;
    crd::containers::Array<crd::f64> av(alloc);
    crd::containers::Array<crd::f64> atu(alloc);
    av.resize(m);
    atu.resize(n);
    for (crd::u32 k = 0; k < cnt; ++k)
    {
        const crd::f64 sigma = r.values[k];
        const crd::f64* uk = r.left_vectors.data() + static_cast<crd::usize>(k) * m;
        const crd::f64* vk = r.right_vectors.data() + static_cast<crd::usize>(k) * n;
        (void)op.apply({vk, n}, {av.data(), m});           // A·v
        (void)op.apply_adjoint({uk, m}, {atu.data(), n});  // Aᵀ·u
        crd::f64 e1 = 0.0;
        crd::f64 nu = 0.0;
        for (crd::u32 i = 0; i < m; ++i)
        {
            const crd::f64 d = av[i] - sigma * uk[i];
            e1 += d * d;
            nu += uk[i] * uk[i];
        }
        crd::f64 e2 = 0.0;
        crd::f64 nv = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::f64 d = atu[i] - sigma * vk[i];
            e2 += d * d;
            nv += vk[i] * vk[i];
        }
        CHECK(std::sqrt(e1) < 1e-6); // A·v = σ·u
        CHECK(std::sqrt(e2) < 1e-6); // Aᵀ·u = σ·v (the coupled sign — independent re-pin would break this)
        CHECK(std::fabs(nu - 1.0) < 1e-9); // ‖u‖ = 1
        CHECK(std::fabs(nv - 1.0) < 1e-9); // ‖v‖ = 1
    }
}
} // namespace

TEST_CASE("v6-h sparse SVD (GKL core): largest singular triplets of a tall A vs the dense svd oracle",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 m = 60;
    const crd::u32 n = 40;
    Csr a = tall_sparse(&alloc, m, n);
    sp::SparseLinearOp<crd::f64> op(a);

    // Dense oracle: SVD of the same A.
    dn::Matrix<crd::f64> ad = tall_dense(&alloc, m, n);
    dn::SVD<crd::f64> ref = dn::svd<crd::f64>(&alloc, ad);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.tol = 1e-9;
    opts.ncv = 24; // < min(m,n)=40 ⇒ genuinely iterative
    auto r = eig::svds<crd::f64>(op, opts, &alloc);

    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged);
    for (crd::u32 k = 0; k < 4; ++k)
    {
        CHECK(std::fabs(r.values[k] - ref.s.data()[k]) < 1e-7); // largest 4 singular values vs dense oracle
        CHECK(r.residuals[k] < 1e-6);
    }
    check_triplets(r, op, 4, &alloc);
}

TEST_CASE("v6-h sparse SVD f32: largest singular triplets vs the dense oracle (relaxed tol)",
          "[hesap][eigen][v6]")
{
    // f32 is instantiated; exercise it (same template, lower precision ⇒ relaxed tol). Self-contained f32 build.
    using F = crd::f32;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 m = 24;
    const crd::u32 n = 16;
    auto diag = [](crd::u32 i) -> F {
        return i < 4 ? std::pow(2.0F, 6.0F - static_cast<F>(i)) : 2.0F - 0.02F * static_cast<F>(i);
    };
    sp::TripletBuilder<F> tb(&alloc, m, n);
    dn::Matrix<F> ad(&alloc, m, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, diag(i));
        ad.at(i, i) = diag(i);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0F);
            ad.at(i, i + 1) = -1.0F;
        }
    }
    for (crd::u32 i = n; i < m; ++i)
    {
        tb.add(i, i - n, 0.3F);
        ad.at(i, i - n) = 0.3F;
    }
    sp::SparseMatrix<F, sp::SparseFormat::Csr> a = tb.compress();
    sp::SparseLinearOp<F> op(a);
    dn::SVD<F> ref = dn::svd<F>(&alloc, ad);

    eig::EigenOptions<F> opts;
    opts.nev = 4;
    opts.tol = 1e-3F;
    opts.ncv = 12;
    auto r = eig::svds<F>(op, opts, &alloc);
    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged);
    for (crd::u32 k = 0; k < 4; ++k)
    {
        CHECK(std::fabs(r.values[k] - ref.s.data()[k]) < 1e-2F); // largest 4 vs dense oracle, f32 precision
    }
}

TEST_CASE("v6-h sparse SVD thick restart converges a CLOSELY-SPACED spectrum at bounded ncv",
          "[hesap][eigen][v6]")
{
    // The graded spectrum's top 4 are closely spaced (gaps ~0.5) — no-restart GKL at this ncv canNOT converge
    // them (diagnosed during development), but thick restart does. Asserting iterations ≥ 2 proves restart engaged.
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 m = 60;
    const crd::u32 n = 40;
    Csr a = graded_sparse(&alloc, m, n);
    sp::SparseLinearOp<crd::f64> op(a);
    dn::Matrix<crd::f64> ad = graded_dense(&alloc, m, n);
    dn::SVD<crd::f64> ref = dn::svd<crd::f64>(&alloc, ad);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.tol = 1e-9;
    opts.ncv = 20;        // bounded, < min(m,n)=40 ⇒ no-restart cannot converge the closely-spaced top 4
    opts.max_restarts = 300;
    auto r = eig::svds<crd::f64>(op, opts, &alloc);

    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged);
    REQUIRE(r.iterations >= 2); // multiple restart cycles ran — the restart is what made convergence possible
    for (crd::u32 k = 0; k < 4; ++k)
    {
        CHECK(std::fabs(r.values[k] - ref.s.data()[k]) < 1e-6);
        CHECK(r.residuals[k] < 1e-6);
    }
    check_triplets(r, op, 4, &alloc);
}

TEST_CASE("v6-h sparse SVD determinism moat {1,2,4,8} (both spmv directions forced parallel)",
          "[hesap][eigen][v6][moat]")
{
    // ParallelSpmvLeastSquaresOp forces BOTH A·v and Aᵀ·u onto the parallel SELL spmv (min_stored_bytes=0) —
    // both are bit-exact across worker counts (the only parallel reductions in IRLBA: there is no factor). The
    // spiked spectrum ⇒ well-separated ⇒ unique singular vectors (moat ground rule).
    const crd::u32 m = 60;
    const crd::u32 n = 40;
    crd::memory::TlsfAllocator alloc(1U << 26);
    Csr a = tall_sparse(&alloc, m, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.tol = 1e-9;
    opts.ncv = 24;

    crd::containers::Array<crd::f64> sval_ref(&alloc);
    crd::containers::Array<crd::f64> u_ref(&alloc);
    crd::containers::Array<crd::f64> v_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSpmvLeastSquaresOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::svds<crd::f64>(op, opts, &alloc);
            REQUIRE(r.values.size() == 4);
            if (!have_ref)
            {
                sval_ref.resize(r.values.size());
                for (crd::u32 s = 0; s < r.values.size(); ++s)
                {
                    sval_ref[s] = r.values[s];
                }
                u_ref.resize(r.left_vectors.size());
                for (crd::usize i = 0; i < r.left_vectors.size(); ++i)
                {
                    u_ref[i] = r.left_vectors[i];
                }
                v_ref.resize(r.right_vectors.size());
                for (crd::usize i = 0; i < r.right_vectors.size(); ++i)
                {
                    v_ref[i] = r.right_vectors[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (crd::u32 s = 0; s < r.values.size() && ident; ++s)
                {
                    ident = (r.values[s] == sval_ref[s]);
                }
                for (crd::usize i = 0; i < r.left_vectors.size() && ident; ++i)
                {
                    ident = (r.left_vectors[i] == u_ref[i]);
                }
                for (crd::usize i = 0; i < r.right_vectors.size() && ident; ++i)
                {
                    ident = (r.right_vectors[i] == v_ref[i]);
                }
                CHECK(ident); // σ, U, V bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
