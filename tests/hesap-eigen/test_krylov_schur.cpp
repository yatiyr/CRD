// crd-hesap-eigen v6-c (completion) — KRYLOV-SCHUR restart: bounded-memory NONSYMMETRIC eigensolver.
// Rotation-block matrix [[a,b],[−b,a]] ⇒ analytically KNOWN eigenvalues a±b·i. At ncv ≪ n the no-restart
// Arnoldi cannot resolve the wanted in one cycle; Krylov-Schur refines them through deterministic
// reorder-Schur restarts. The moat: values + complex eigenvectors bit-identical across {1,2,4,8}.

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
#include <functional>

namespace eig = crd::hesap::eigen;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// nblk 2×2 rotation blocks: block k = [[a_k, b],[−b, a_k]] ⇒ eigenvalues a_k ± b·i. a_k = nblk − k (Re
// descending from nblk). The blocks are placed on the diagonal of an n=2·nblk sparse matrix.
Csr rotation_blocks(crd::memory::IAllocator* alloc, crd::u32 nblk, double b)
{
    const crd::u32 n = 2 * nblk;
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    for (crd::u32 k = 0; k < nblk; ++k)
    {
        const double a = static_cast<double>(nblk - k);
        const crd::u32 r = 2 * k;
        tb.add(r, r, a);
        tb.add(r + 1, r + 1, a);
        tb.add(r, r + 1, b);
        tb.add(r + 1, r, -b);
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v6-c Krylov-Schur converges the top LargestReal eigenvalues at ncv much smaller than n",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 nblk = 100; // n = 200
    const double b = 0.5;
    Csr a = rotation_blocks(&alloc, nblk, b);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.ncv = 20; // ncv = 20 << n = 200 ⇒ restarts required
    opts.which = eig::Which::LargestReal;
    opts.tol = 1e-9;

    sp::SparseLinearOp<crd::f64> op(a);
    auto r = eig::eigs_nonsym_ks<crd::f64>(op, opts, &alloc);
    REQUIRE(r.values.size() == 4);
    REQUIRE(r.iterations > opts.ncv); // matvecs exceeded one cycle ⇒ a restart actually engaged

    // Each block a_k gives the CONJUGATE PAIR a_k ± 0.5i (same real part), so the top 4 by LargestReal are the
    // top TWO blocks' pairs: {100±0.5i, 99±0.5i} ⇒ Re = 100,100,99,99 and |Im| = 0.5 for all four.
    double re[4];
    double maxres = 0.0;
    for (crd::u32 s = 0; s < 4; ++s)
    {
        re[s] = r.values[s].re;
        CHECK(std::fabs(std::fabs(r.values[s].im) - b) < 1e-7);
        maxres = std::max(maxres, r.residuals[s]);
    }
    std::sort(re, re + 4, std::greater<>());
    INFO("max residual = " << maxres << "  matvecs = " << r.iterations << "  Re = " << re[0] << "," << re[1] << ","
                           << re[2] << "," << re[3]);
    CHECK(std::fabs(re[0] - 100.0) < 1e-7);
    CHECK(std::fabs(re[1] - 100.0) < 1e-7);
    CHECK(std::fabs(re[2] - 99.0) < 1e-7);
    CHECK(std::fabs(re[3] - 99.0) < 1e-7);
    CHECK(maxres < 1e-7);
    CHECK(r.converged);
}

TEST_CASE("v6-c Krylov-Schur reaches the full nev request at bounded ncv and never regresses vs no-restart",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr a = rotation_blocks(&alloc, 100, 0.5); // n = 200

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.ncv = 20;
    opts.which = eig::Which::LargestReal;
    opts.tol = 1e-9;

    sp::SparseLinearOp<crd::f64> op(a);
    auto rno = eig::eigs_nonsym<crd::f64>(op, opts, &alloc);   // one-shot, bounded m
    auto rks = eig::eigs_nonsym_ks<crd::f64>(op, opts, &alloc); // restarted
    INFO("no-restart nconv = " << rno.nconv << "  Krylov-Schur nconv = " << rks.nconv);
    CHECK(rks.nconv >= rno.nconv); // restart never converges fewer
    CHECK(rks.nconv == opts.nev);  // and reaches the full request
}

TEST_CASE("v6-c Krylov-Schur determinism moat {1,2,4,8}", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    Csr a = rotation_blocks(&alloc, 60, 0.5); // n = 120

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.ncv = 20;
    opts.which = eig::Which::LargestReal;
    opts.tol = 1e-9;

    crd::containers::Array<crd::f64> ref(&alloc);   // interleaved re/im of the 4 wanted values
    crd::containers::Array<crd::f64> vref(&alloc);  // eigenvector real parts
    crd::containers::Array<crd::f64> viref(&alloc); // eigenvector imaginary parts
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::eigs_nonsym_ks<crd::f64>(op, opts, &alloc);
            REQUIRE(r.values.size() == 4);
            REQUIRE(r.vectors_im.size() == r.vectors.size());
            if (!have_ref)
            {
                ref.resize(static_cast<crd::usize>(r.values.size()) * 2);
                for (crd::u32 s = 0; s < r.values.size(); ++s)
                {
                    ref[2 * s] = r.values[s].re;
                    ref[2 * s + 1] = r.values[s].im;
                }
                vref.resize(r.vectors.size());
                viref.resize(r.vectors_im.size());
                for (crd::usize i = 0; i < r.vectors.size(); ++i)
                {
                    vref[i] = r.vectors[i];
                    viref[i] = r.vectors_im[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (crd::u32 s = 0; s < r.values.size() && ident; ++s)
                {
                    ident = (r.values[s].re == ref[2 * s]) && (r.values[s].im == ref[2 * s + 1]);
                }
                for (crd::usize i = 0; i < r.vectors.size() && ident; ++i)
                {
                    ident = (r.vectors[i] == vref[i]) && (r.vectors_im[i] == viref[i]);
                }
                CHECK(ident); // values + complex eigenvectors bit-identical THROUGH the restart cycles
            }
        }
        crd::jobs::shutdown();
    }
}
