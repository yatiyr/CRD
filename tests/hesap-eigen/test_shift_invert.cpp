// crd-hesap-eigen v6-d — shift-invert: INTERIOR eigenvalues nearest a shift σ (the v6-a/b methods cannot
// target the interior from the spectrum ends) via a v5 partial-pivot factor of (A−σI) + thick-restart Lanczos.

#include <crd/containers/array.hpp>
#include <crd/hesap/eigen/eigen.hpp>
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

TEST_CASE("v6-d shift-invert finds the INTERIOR Laplacian eigenvalues nearest sigma", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 n = 64;
    Csr a = laplacian_1d(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.tol = 1e-10;
    // sigma = 2.0 sits in the MIDDLE of the spectrum [~0, ~4]; the 4 nearest are λ_31..34.
    auto r = eig::eigs_sym_shift_invert<crd::f64>(a, 2.0, opts, &alloc);
    REQUIRE(r.values.size() == 4);

    double got[4];
    double maxres = 0.0;
    for (crd::u32 s = 0; s < 4; ++s)
    {
        got[s] = r.values[s].re;
        maxres = std::max(maxres, r.residuals[s]);
    }
    std::sort(got, got + 4);
    double exp[4] = {lambda_k(n, 31), lambda_k(n, 32), lambda_k(n, 33), lambda_k(n, 34)};
    std::sort(exp, exp + 4);
    INFO("max residual = " << maxres << "  got = " << got[0] << "," << got[1] << "," << got[2] << "," << got[3]
                           << "  exp = " << exp[0] << "," << exp[1] << "," << exp[2] << "," << exp[3]);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(got[s] - exp[s]) < 1e-7); // exact interior eigenvalues recovered
    }
    CHECK(maxres < 1e-8);
}

TEST_CASE("v6-d shift-invert determinism moat {1,2,4,8}", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 n = 120;
    Csr a = laplacian_1d(&alloc, n);
    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
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
            // The (A−σI) factor is built with `nw` workers (the v5f-e moat); the eigensolve on top is serial.
            auto r = eig::eigs_sym_shift_invert<crd::f64>(a, 2.0, opts, &alloc, nw);
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
                CHECK(ident);
            }
        }
        crd::jobs::shutdown();
    }
}
