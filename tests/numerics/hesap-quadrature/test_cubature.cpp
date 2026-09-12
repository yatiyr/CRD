// crd-hesap-quadrature v13-k - multi-D cubature (tensor-Gauss + Genz-Malik adaptive), gated bit-close to
// scipy.integrate.cubature + analytic + tensor-Gauss polynomial exactness + determinism + error-tier contract.

#include <crd/hesap/quadrature/quadrature.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace q = crd::hesap::quadrature;
using crd::f64;
using crd::containers::ConstSpan;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
} // namespace

TEST_CASE("v13-k: Genz-Malik adaptive cubature - bit-close to scipy.integrate.cubature + analytic",
          "[v13-k][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // 3D: int_0^1 (cos x1 + cos x2 + cos x3) = 3 sin(1)
    {
        const f64 a[3] = {0.0, 0.0, 0.0};
        const f64 b[3] = {1.0, 1.0, 1.0};
        const auto r = q::integrate_cubature<f64>(
            &alloc, [](const f64* x) { return std::cos(x[0]) + std::cos(x[1]) + std::cos(x[2]); }, ConstSpan<f64>{a, 3},
            ConstSpan<f64>{b, 3}, 1e-10, 1e-10);
        CHECK(r.ok());
        CHECK(close(r.value, 3.0 * std::sin(1.0), 1e-9, 1e-11));
        CHECK(r.eval_count > 0);
        CHECK(r.subdiv_count >= 1);
    }
    // 2D gaussian bump: int_-1^1 exp(-(x^2+y^2)) = (sqrt(pi) erf(1))^2
    {
        const f64 a[2] = {-1.0, -1.0};
        const f64 b[2] = {1.0, 1.0};
        const auto r = q::integrate_cubature<f64>(
            &alloc, [](const f64* x) { return std::exp(-(x[0] * x[0] + x[1] * x[1])); }, ConstSpan<f64>{a, 2},
            ConstSpan<f64>{b, 2}, 1e-11, 1e-11);
        const f64 ref = std::pow(std::sqrt(3.14159265358979323846) * std::erf(1.0), 2.0);
        CHECK(close(r.value, ref, 1e-9, 1e-11));
    }
    // 5D: int_[0,1]^5 prod(1+x_i)? use sum x_i^2 = 5 * (1/3) * 1 = 5/3 (each axis integrates x^2 over [0,1] then *1^4)
    {
        const f64 a[5] = {0, 0, 0, 0, 0};
        const f64 b[5] = {1, 1, 1, 1, 1};
        const auto r = q::integrate_cubature<f64>(
            &alloc, [](const f64* x) { return x[0] * x[0] + x[1] * x[1] + x[2] * x[2] + x[3] * x[3] + x[4] * x[4]; },
            ConstSpan<f64>{a, 5}, ConstSpan<f64>{b, 5}, 1e-10, 1e-10);
        CHECK(close(r.value, 5.0 / 3.0, 1e-9, 1e-11));
    }
    // determinism
    const f64 a[2] = {0.0, 0.0};
    const f64 b[2] = {1.0, 1.0};
    auto fn = [](const f64* x)
    {
        return std::exp(x[0] * x[1]);
    };
    const auto r1 = q::integrate_cubature<f64>(&alloc, fn, ConstSpan<f64>{a, 2}, ConstSpan<f64>{b, 2}, 1e-10, 1e-10);
    const auto r2 = q::integrate_cubature<f64>(&alloc, fn, ConstSpan<f64>{a, 2}, ConstSpan<f64>{b, 2}, 1e-10, 1e-10);
    CHECK(r1.value == r2.value);
    // bad input: d < 2
    const f64 a1[1] = {0.0};
    const f64 b1[1] = {1.0};
    CHECK(q::integrate_cubature<f64>(
              &alloc, [](const f64* x) { return x[0]; }, ConstSpan<f64>{a1, 1}, ConstSpan<f64>{b1, 1}, 1e-10, 1e-10)
              .status == q::QuadStatus::BadInput);
}

TEST_CASE("v13-k: tensor-product Gauss - polynomial exactness + analytic", "[v13-k][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // int_[0,2]x[0,3] x*y = (int_0^2 x)(int_0^3 y) = 2 * 4.5 = 9, exact with n=2
    {
        const f64 a[2] = {0.0, 0.0};
        const f64 b[2] = {2.0, 3.0};
        const auto r = q::integrate_tensor_gauss<f64>(
            &alloc, [](const f64* x) { return x[0] * x[1]; }, ConstSpan<f64>{a, 2}, ConstSpan<f64>{b, 2}, 2);
        CHECK(close(r.value, 9.0, 1e-13, 1e-13));
    }
    // exactness: int_[0,1]^2 x^5 y^5 = 1/36, exact for n=3 (degree 5 <= 2*3-1)
    {
        const f64 a[2] = {0.0, 0.0};
        const f64 b[2] = {1.0, 1.0};
        const auto r = q::integrate_tensor_gauss<f64>(
            &alloc, [](const f64* x) { return std::pow(x[0], 5) * std::pow(x[1], 5); }, ConstSpan<f64>{a, 2},
            ConstSpan<f64>{b, 2}, 3);
        CHECK(close(r.value, 1.0 / 36.0, 1e-12, 1e-13));
    }
    // 3D smooth: int_[0,1]^3 exp(x+y+z) = (e-1)^3, tensor-Gauss n=8 high accuracy
    {
        const f64 a[3] = {0.0, 0.0, 0.0};
        const f64 b[3] = {1.0, 1.0, 1.0};
        const auto r = q::integrate_tensor_gauss<f64>(
            &alloc, [](const f64* x) { return std::exp(x[0] + x[1] + x[2]); }, ConstSpan<f64>{a, 3},
            ConstSpan<f64>{b, 3}, 8);
        CHECK(close(r.value, std::pow(std::exp(1.0) - 1.0, 3), 1e-12, 1e-13));
        CHECK(r.eval_count == 512U); // 8^3
    }
}

TEST_CASE("v13-k: Lebedev sphere cubature - spherical-harmonic exactness", "[v13-k][quadrature]")
{
    constexpr f64 four_pi = 4.0 * 3.14159265358979323846;
    // int_{S^2} 1 dOmega = 4pi
    CHECK(close(q::integrate_lebedev<f64>([](f64, f64, f64) { return 1.0; }, 7).value, four_pi, 1e-12, 1e-12));
    // int x^2 dOmega = 4pi/3
    CHECK(
        close(q::integrate_lebedev<f64>([](f64 x, f64, f64) { return x * x; }, 7).value, four_pi / 3.0, 1e-11, 1e-12));
    // int x^2 y^2 z^2 dOmega = 4pi/105 (degree-6 polynomial, exact for a degree-7 rule)
    CHECK(close(q::integrate_lebedev<f64>([](f64 x, f64 y, f64 z) { return x * x * y * y * z * z; }, 7).value,
                four_pi / 105.0, 1e-11, 1e-13));
    // a degree-1 spherical harmonic integrates to ~0 (odd parity)
    CHECK(std::abs(q::integrate_lebedev<f64>([](f64 x, f64, f64) { return x; }, 11).value) < 1e-12);
    // higher degree resolves x^8 (degree 8): exact for degree-11 rule; int x^8 dOmega = 4pi/9
    CHECK(close(q::integrate_lebedev<f64>([](f64 x, f64, f64) { return std::pow(x, 8); }, 11).value, four_pi / 9.0,
                1e-10, 1e-12));
    CHECK(q::integrate_lebedev<f64>([](f64, f64, f64) { return 1.0; }, 4).status == q::QuadStatus::BadInput);
}

TEST_CASE("v13-k: Dunavant simplex cubature - polynomial exactness on triangles", "[v13-k][quadrature]")
{
    // reference triangle (0,0),(1,0),(0,1): int x^a y^b = a!b!/(a+b+2)!
    auto reftri = [](auto&& f, int deg)
    {
        return q::integrate_triangle<f64>(f, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, deg).value;
    };
    CHECK(close(reftri([](f64, f64) { return 1.0; }, 1), 0.5, 1e-13, 1e-14));                  // area
    CHECK(close(reftri([](f64 x, f64) { return x; }, 2), 1.0 / 6.0, 1e-13, 1e-14));            // 1!0!/3!
    CHECK(close(reftri([](f64 x, f64 y) { return x * x * y; }, 4), 1.0 / 60.0, 1e-13, 1e-14)); // 2!1!/5!
    CHECK(close(reftri([](f64 x, f64 y) { return std::pow(x, 3) * std::pow(y, 2); }, 6), 6.0 * 2.0 / 5040.0, 1e-13,
                1e-14)); // 3!2!/7!
    // general triangle (0,0),(2,0),(0,3): area = 3, int 1 = 3
    CHECK(close(q::integrate_triangle<f64>([](f64, f64) { return 1.0; }, 0.0, 0.0, 2.0, 0.0, 0.0, 3.0, 3).value, 3.0,
                1e-13, 1e-13));
    CHECK(q::integrate_triangle<f64>([](f64, f64) { return 1.0; }, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 9).status ==
          q::QuadStatus::BadInput);
}

TEST_CASE("v13-k: Smolyak sparse grid - exactness + tensor cross-check + determinism", "[v13-k][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const f64 am1[3] = {-1.0, -1.0, -1.0};
    const f64 bp1[3] = {1.0, 1.0, 1.0};
    // d=2 polynomial exactness: int_[-1,1]^2 x^2 y^2 = 4/9 (exact)
    {
        const auto grid = q::build_smolyak_grid<f64>(&alloc, 2, 4);
        const auto r = q::integrate_smolyak<f64>(
            grid, [](const f64* x) { return x[0] * x[0] * x[1] * x[1]; }, ConstSpan<f64>{am1, 2},
            ConstSpan<f64>{bp1, 2});
        CHECK(close(r.value, 4.0 / 9.0, 1e-12, 1e-13));
    }
    // d=3 smooth, converges to (e - 1/e)^3; sparse grid uses far fewer points than tensor 2^q^3
    {
        const auto grid = q::build_smolyak_grid<f64>(&alloc, 3, 8);
        const auto r = q::integrate_smolyak<f64>(
            grid, [](const f64* x) { return std::exp(x[0] + x[1] + x[2]); }, ConstSpan<f64>{am1, 3},
            ConstSpan<f64>{bp1, 3});
        const f64 ref = std::pow(std::exp(1.0) - std::exp(-1.0), 3);
        CHECK(close(r.value, ref, 1e-5, 1e-7));
        CHECK(r.eval_count < 1000U); // sparse: << (2^7+1)^3 = 2.1M tensor points
        // determinism: same grid, run twice
        const auto r2 = q::integrate_smolyak<f64>(
            grid, [](const f64* x) { return std::exp(x[0] + x[1] + x[2]); }, ConstSpan<f64>{am1, 3},
            ConstSpan<f64>{bp1, 3});
        CHECK(r.value == r2.value);
    }
    // bad input: d out of range
    CHECK(!q::build_smolyak_grid<f64>(&alloc, 1, 4).valid);
}
