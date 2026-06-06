// crd-hesap-eigen v6-c — nonsymmetric Arnoldi: complex eigenvalues of 2×2 rotation blocks [[a,b],[−b,a]]
// (eigenvalues a±b·i — KNOWN) + the {1,2,4,8}-worker moat (complex Ritz values bit-identical).

#include <crd/containers/array.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace eig = crd::hesap::eigen;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// Block-diagonal of `nblk` 2×2 rotation blocks: block i = [[a_i, b_i], [−b_i, a_i]] with a_i = i+1, b_i = 0.5
// ⇒ eigenvalues (i+1) ± 0.5·i. Distinct real parts ⇒ well-separated by `LargestReal`; the minimal polynomial
// has degree 2·nblk ⇒ full Arnoldi at m = n is exact.
Csr rotation_blocks(crd::memory::IAllocator* a, crd::u32 nblk)
{
    const crd::u32 n = 2 * nblk;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < nblk; ++i)
    {
        const crd::u32 o = 2 * i;
        const double ai = static_cast<double>(i + 1);
        const double bi = 0.5;
        tb.add(o, o, ai);
        tb.add(o, o + 1, bi);
        tb.add(o + 1, o, -bi);
        tb.add(o + 1, o + 1, ai);
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v6-c nonsymmetric Arnoldi recovers the complex eigenvalues of rotation blocks", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 nblk = 8; // n=16 ⇒ default ncv=20 clamps to n ⇒ full Arnoldi ⇒ exact
    Csr a = rotation_blocks(&alloc, nblk);
    sp::SparseLinearOp<crd::f64> op(a);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::LargestReal; // top 2 blocks: Re ∈ {8,8,7,7}, |Im| = 0.5
    auto r = eig::eigs_nonsym<crd::f64>(op, opts, &alloc);
    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged);
    CHECK(std::fabs(r.values[0].re - 8.0) < 1e-8);
    CHECK(std::fabs(r.values[1].re - 8.0) < 1e-8);
    CHECK(std::fabs(r.values[2].re - 7.0) < 1e-8);
    CHECK(std::fabs(r.values[3].re - 7.0) < 1e-8);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(std::fabs(r.values[s].im) - 0.5) < 1e-8); // the ±0.5·i conjugate pairs
        CHECK(r.residuals[s] < 1e-7);
    }
}

TEST_CASE("v6-c nonsymmetric Arnoldi determinism moat {1,2,4,8}", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 nblk = 8; // n=16
    Csr a = rotation_blocks(&alloc, nblk);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::LargestReal;

    crd::containers::Array<crd::f64> ref(&alloc);    // interleaved re/im of the 4 wanted values
    crd::containers::Array<crd::f64> vref(&alloc);   // eigenvector real parts
    crd::containers::Array<crd::f64> viref(&alloc);  // eigenvector imaginary parts
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::eigs_nonsym<crd::f64>(op, opts, &alloc);
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
                    ident = (r.vectors[i] == vref[i]) && (r.vectors_im[i] == viref[i]); // re AND im
                }
                CHECK(ident); // complex Ritz VALUES + EIGENVECTORS (Re+Im) bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
