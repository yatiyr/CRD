// crd-hesap-quadrature v13-g — the integrate() API + result contract + Lobatto/Radau nodes + composite rules +
// Newton-Cotes, gated vs scipy + polynomial-exactness degree + the positive-weight invariant + determinism.

#include <crd/hesap/quadrature/quadrature.hpp>

#include "integrate_refs.inc"

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace q = crd::hesap::quadrature;
using crd::containers::ConstSpan;
using crd::f64;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
// ∫_{-1}^1 x^k dx
f64 mono_exact(int k) noexcept
{
    return (k % 2 == 1) ? 0.0 : 2.0 / (k + 1);
}
} // namespace

TEST_CASE("v13-g: Gauss-Lobatto / Gauss-Radau nodes+weights vs scipy + endpoints + positive weights",
          "[v13-g][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    f64                        x[5];
    f64                        w[5];

    q::gauss_lobatto<f64>(&alloc, 5, x, w);
    for (int i = 0; i < 5; ++i)
    {
        INFO("lobatto i=" << i);
        CHECK(close(x[i], ref_lob5_x[i], 1e-13, 1e-14));
        CHECK(close(w[i], ref_lob5_w[i], 1e-13, 1e-14));
        CHECK(w[i] > 0.0); // positive-weight invariant
    }
    CHECK(x[0] == -1.0); // includes both endpoints
    CHECK(x[4] == 1.0);

    q::gauss_radau<f64>(&alloc, 5, x, w);
    for (int i = 0; i < 5; ++i)
    {
        INFO("radau i=" << i);
        CHECK(close(x[i], ref_rad5_x[i], 1e-13, 1e-14));
        CHECK(close(w[i], ref_rad5_w[i], 1e-13, 1e-14));
        CHECK(w[i] > 0.0);
    }
    CHECK(x[0] == -1.0); // includes the left endpoint
}

TEST_CASE("v13-g: Lobatto/Radau exactness degree (<=2n-3 / <=2n-2) + Gauss <=2n-1", "[v13-g][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    for (int n : {3, 5, 8})
    {
        for (int k = 0; k <= 2 * n - 1; ++k) // Gauss exact ≤ 2n−1
        {
            const auto r = q::integrate_gauss<f64>(
                &alloc, [k](f64 x) { return std::pow(x, k); }, -1.0, 1.0, n);
            INFO("gauss n=" << n << " k=" << k);
            CHECK(close(r.value, mono_exact(k), 1e-12, 1e-13));
        }
        for (int k = 0; k <= 2 * n - 3; ++k) // Lobatto exact ≤ 2n−3
        {
            const auto r = q::integrate_lobatto<f64>(
                &alloc, [k](f64 x) { return std::pow(x, k); }, -1.0, 1.0, n);
            INFO("lobatto n=" << n << " k=" << k);
            CHECK(close(r.value, mono_exact(k), 1e-12, 1e-13));
        }
        for (int k = 0; k <= 2 * n - 2; ++k) // Radau exact ≤ 2n−2
        {
            const auto r = q::integrate_radau<f64>(
                &alloc, [k](f64 x) { return std::pow(x, k); }, -1.0, 1.0, n);
            INFO("radau n=" << n << " k=" << k);
            CHECK(close(r.value, mono_exact(k), 1e-12, 1e-13));
        }
    }
}

TEST_CASE("v13-g: integrate() converges on a smooth function + affine map + result contract + bad input",
          "[v13-g][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    constexpr f64              e_minus_1 = 1.7182818284590452; // ∫_0^1 e^x dx
    auto                       fexp      = [](f64 x) { return std::exp(x); };

    const auto g = q::integrate_gauss<f64>(&alloc, fexp, 0.0, 1.0, 12);
    CHECK(g.ok());
    CHECK(g.eval_count == 12);
    CHECK(g.error_estimate == 0.0); // fixed rule: no self-estimate (honest Tier-0)
    CHECK(close(g.value, e_minus_1, 1e-13, 1e-14));
    CHECK(close(q::integrate_lobatto<f64>(&alloc, fexp, 0.0, 1.0, 12).value, e_minus_1, 1e-12, 1e-13));
    CHECK(close(q::integrate_radau<f64>(&alloc, fexp, 0.0, 1.0, 12).value, e_minus_1, 1e-12, 1e-13));

    // integrate_symmetric (precomputed Gauss nodes) = the general apply, to ≤1e-13 (symmetric-pair fast path).
    crd::containers::Array<f64> sx(&alloc);
    crd::containers::Array<f64> sw(&alloc);
    sx.resize(12);
    sw.resize(12);
    q::gauss_legendre<f64>(&alloc, 12, sx.data(), sw.data());
    const auto sym = q::integrate_symmetric<f64>(fexp, 0.0, 1.0, ConstSpan<f64>{sx.data(), 12},
                                                 ConstSpan<f64>{sw.data(), 12});
    CHECK(close(sym.value, e_minus_1, 1e-13, 1e-14));
    CHECK(close(sym.value, g.value, 1e-13, 1e-14));

    // affine map: ∫_a^b 1 dx = b−a
    CHECK(close(q::integrate_gauss<f64>(&alloc, [](f64) { return 1.0; }, 2.0, 7.5, 4).value, 5.5, 1e-14, 1e-14));

    // bad input
    CHECK(q::integrate_gauss<f64>(&alloc, fexp, 0.0, 1.0, 0).status == q::QuadStatus::BadInput);
    CHECK(q::integrate_lobatto<f64>(&alloc, fexp, 0.0, 1.0, 1).status == q::QuadStatus::BadInput);

    // determinism: bit-identical re-evaluation
    CHECK(q::integrate_gauss<f64>(&alloc, fexp, 0.0, 1.0, 12).value
          == q::integrate_gauss<f64>(&alloc, fexp, 0.0, 1.0, 12).value);
}

TEST_CASE("v13-g: Newton-Cotes weights vs scipy + sum-to-n invariant", "[v13-g][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    f64                        w[16];
    const double*              refs[] = {nullptr, nullptr, ref_nc2, ref_nc3, ref_nc4, nullptr, ref_nc6, nullptr, ref_nc8};
    for (int n : {2, 3, 4, 6, 8})
    {
        q::newton_cotes<f64>(&alloc, n, w);
        f64 sum = 0.0;
        for (int i = 0; i <= n; ++i)
        {
            INFO("nc n=" << n << " i=" << i);
            CHECK(close(w[i], refs[n][i], 1e-9, 1e-11));
            sum += w[i];
        }
        CHECK(close(sum, static_cast<f64>(n), 1e-10, 1e-12)); // ∫_0^n 1 dx = n
    }
}

TEST_CASE("v13-g: composite trapezoid + Simpson on samples vs scipy (both parities) + non-uniform + determinism",
          "[v13-g][quadrature]")
{
    // Simpson — odd sample count (even intervals): standard; even sample count (odd intervals): scipy's correction.
    CHECK(close(q::simpson<f64>(ConstSpan<f64>{ref_samp_odd, 9}, ref_dx_odd), ref_simpson_odd, 1e-13, 1e-14));
    CHECK(close(q::simpson<f64>(ConstSpan<f64>{ref_samp_even, 8}, ref_dx_even), ref_simpson_even, 1e-13, 1e-14));
    // trapezoid — uniform + non-uniform
    CHECK(close(q::trapezoid<f64>(ConstSpan<f64>{ref_samp_odd, 9}, ref_dx_odd), ref_trap_odd, 1e-13, 1e-14));
    CHECK(close(q::trapezoid<f64>(ConstSpan<f64>{ref_nu_y, 6}, ConstSpan<f64>{ref_nu_x, 6}), ref_trap_nu, 1e-13, 1e-14));
    // determinism
    CHECK(q::simpson<f64>(ConstSpan<f64>{ref_samp_even, 8}, ref_dx_even)
          == q::simpson<f64>(ConstSpan<f64>{ref_samp_even, 8}, ref_dx_even));
}
