// crd-hesap-quadrature v12-c — Gauss quadrature (Golub-Welsch) gated vs scipy roots_* + degree-(2n-1) exactness.

#include <crd/hesap/quadrature/quadrature.hpp>

#include "gauss_refs.inc"

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace q = crd::hesap::quadrature;
using crd::f64;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
constexpr int kN = 8;
} // namespace

TEST_CASE("gauss: Legendre/Hermite/Laguerre/Jacobi/Gegenbauer/Chebyshev nodes+weights vs scipy",
          "[v12-c][quadrature][gauss]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    f64 x[kN];
    f64 w[kN];

    q::gauss_legendre<f64>(&alloc, kN, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("legendre i=" << i);
        CHECK(close(x[i], ref_gl_x[i], 1e-12, 1e-13));
        CHECK(close(w[i], ref_gl_w[i], 1e-12, 1e-13));
    }
    q::gauss_hermite<f64>(&alloc, kN, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("hermite i=" << i);
        CHECK(close(x[i], ref_gh_x[i], 1e-12, 1e-13));
        CHECK(close(w[i], ref_gh_w[i], 1e-12, 1e-13));
    }
    q::gauss_laguerre<f64>(&alloc, kN, 0.0, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("laguerre i=" << i);
        CHECK(close(x[i], ref_gla_x[i], 1e-11, 1e-12));
        CHECK(close(w[i], ref_gla_w[i], 1e-11, 1e-12));
    }
    q::gauss_laguerre<f64>(&alloc, kN, 1.5, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("genlaguerre i=" << i);
        CHECK(close(x[i], ref_ggla_x[i], 1e-11, 1e-12));
        CHECK(close(w[i], ref_ggla_w[i], 1e-11, 1e-12));
    }
    q::gauss_jacobi<f64>(&alloc, kN, 1.5, 0.5, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("jacobi i=" << i);
        CHECK(close(x[i], ref_gj_x[i], 1e-11, 1e-12));
        CHECK(close(w[i], ref_gj_w[i], 1e-11, 1e-12));
    }
    q::gauss_gegenbauer<f64>(&alloc, kN, 0.75, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("gegenbauer i=" << i);
        CHECK(close(x[i], ref_ggeg_x[i], 1e-11, 1e-12));
        CHECK(close(w[i], ref_ggeg_w[i], 1e-11, 1e-12));
    }
    q::gauss_chebyshev_t<f64>(kN, x, w);
    for (int i = 0; i < kN; ++i)
    {
        INFO("chebyshev i=" << i);
        CHECK(close(x[i], ref_gct_x[i], 1e-12, 1e-13));
        CHECK(close(w[i], ref_gct_w[i], 1e-12, 1e-13));
    }
}

// Independent self-check: an n-point Gauss rule integrates polynomials up to degree 2n-1 EXACTLY.
TEST_CASE("gauss: degree-(2n-1) exactness (no external ref)", "[v12-c][quadrature][gauss]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    f64 x[kN];
    f64 w[kN];
    q::gauss_legendre<f64>(&alloc, kN, x, w);
    for (int p = 0; p <= 2 * kN - 1; ++p) // ∫_{-1}^1 x^p dx = 2/(p+1) for even p, 0 for odd
    {
        f64 s = 0.0;
        for (int i = 0; i < kN; ++i)
        {
            s += w[i] * std::pow(x[i], p);
        }
        const f64 exact = (p % 2 == 0) ? 2.0 / (p + 1) : 0.0;
        INFO("legendre moment p=" << p);
        CHECK(close(s, exact, 1e-12, 1e-13));
    }
    q::gauss_hermite<f64>(&alloc, kN, x, w);
    // ∫ x^p e^{-x²} dx = 0 (odd p) ; = Γ((p+1)/2) (even p). Check p=0 (√π) and p=2 (√π/2).
    f64 m0 = 0.0;
    f64 m2 = 0.0;
    for (int i = 0; i < kN; ++i)
    {
        m0 += w[i];
        m2 += w[i] * x[i] * x[i];
    }
    CHECK(close(m0, 1.7724538509055160, 1e-12, 1e-13));      // √π
    CHECK(close(m2, 1.7724538509055160 / 2.0, 1e-12, 1e-13)); // √π/2
}
