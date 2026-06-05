// crd-hesap-direct v5f-(a) STEP 1 — dense within-front partial pivoting kernel (factor_front), in isolation.
//
// The bar: factor_front with pivot_threshold>0 produces a factorization satisfying P·A = L·U (P from the
// recorded ipiv) even when the STATIC diagonal fails (zero/tiny leading pivot) — and pivot_threshold=0
// reproduces the byte-unchanged static path (ipiv = identity, P = I ⇒ L·U = A). Zero blast radius: the
// multifrontal driver does not pass the flag yet (that is STEP 2). Reconstruction is the verification.

#include <crd/containers/array.hpp>
#include <crd/hesap/direct/dense_lu_kernels.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace dir = crd::hesap::direct;

namespace
{
// Factor an n×n dense matrix `a_rowmajor` (row-major) via factor_front (full LU: m=n=npiv) at the given
// pivot threshold, then verify P·A = L·U where P is built from the recorded ipiv.
void check_lu_pp(crd::memory::IAllocator* alloc, crd::u32 n, const double* a_rowmajor, double threshold)
{
    crd::containers::Array<double> d(alloc); // col-major front buffer (leading dim n)
    d.resize(static_cast<crd::usize>(n) * n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            d[static_cast<crd::usize>(j) * n + i] = a_rowmajor[static_cast<crd::usize>(i) * n + j];
        }
    }
    crd::containers::Array<crd::u32> ipiv(alloc);
    ipiv.resize(n);
    const double tiny = std::sqrt(std::numeric_limits<double>::epsilon());
    dir::factor_front<double>(d.data(), n, n, n, n, tiny, nullptr, false, threshold, ipiv.data());

    // Reconstruct L·U (row-major). L unit-lower (L[i][k]=d[k][i], i>k); U upper (U[k][j]=d[j][k], k<=j).
    crd::containers::Array<double> lu(alloc);
    lu.resize(static_cast<crd::usize>(n) * n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            double s = 0.0;
            const crd::u32 kmax = (i < j ? i : j);
            for (crd::u32 k = 0; k <= kmax; ++k)
            {
                double lik = 0.0;
                if (i > k)
                {
                    lik = d[static_cast<crd::usize>(k) * n + i];
                }
                else if (i == k)
                {
                    lik = 1.0;
                }
                const double ukj = d[static_cast<crd::usize>(j) * n + k]; // k <= j here
                s += lik * ukj;
            }
            lu[static_cast<crd::usize>(i) * n + j] = s;
        }
    }

    // P·A: apply the ipiv swaps (in order) to a row-major copy of A.
    crd::containers::Array<double> pa(alloc);
    pa.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize t = 0; t < pa.size(); ++t)
    {
        pa[t] = a_rowmajor[t];
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        const crd::u32 p = ipiv[k];
        if (p != k)
        {
            for (crd::u32 j = 0; j < n; ++j)
            {
                const double tmp = pa[static_cast<crd::usize>(k) * n + j];
                pa[static_cast<crd::usize>(k) * n + j] = pa[static_cast<crd::usize>(p) * n + j];
                pa[static_cast<crd::usize>(p) * n + j] = tmp;
            }
        }
    }

    double err = 0.0;
    for (crd::usize t = 0; t < lu.size(); ++t)
    {
        const double e = lu[t] - pa[t];
        err += e * e;
    }
    CHECK(std::sqrt(err) < 1e-12);
}
} // namespace

TEST_CASE("v5f-a partial pivoting: P·A = L·U when the static diagonal fails (zero leading pivot)", "[hesap][lu-pp][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const double a[9] = {0.0, 2.0, 1.0, 1.0, 1.0, 0.0, 2.0, 1.0, 3.0}; // A[0][0]=0 ⇒ static fails
    check_lu_pp(&alloc, 3, a, 1.0);
}

TEST_CASE("v5f-a partial pivoting: larger system with a zero leading pivot", "[hesap][lu-pp][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const double a[16] = {0.0, 1.0, 2.0, 1.0, 1.0, 5.0, 1.0, 0.0, 2.0, 1.0, 6.0, 1.0, 1.0, 0.0, 1.0, 7.0};
    check_lu_pp(&alloc, 4, a, 1.0);
}

TEST_CASE("v5f-a static path unchanged (threshold 0 ⇒ ipiv identity ⇒ L·U = A)", "[hesap][lu-pp][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const double a[9] = {4.0, 1.0, 0.0, 1.0, 4.0, 1.0, 0.0, 1.0, 4.0}; // strong diagonal — static is fine
    check_lu_pp(&alloc, 3, a, 0.0);
}
