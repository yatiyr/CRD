// test_complex_dual.cpp — Phase 3.1.6 v15-h: complex / Wirtinger forward AD. Gates: holomorphic ops propagate the
// complex derivative via the identical real-dual code AND pass Cauchy-Riemann (∂/∂z̄≈0); non-holomorphic ops
// (conj/Re/|z|/|z|²) yield the correct value AND correctly FAIL CR; the Wirtinger pair (∂/∂z,∂/∂z̄) matches the
// analytic rules AND a 2×2-real-Jacobian finite difference (the validation complex-step CANNOT do); a DFT is linear
// ⇒ holomorphic and its input sensitivity is exact.

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <complex>

using Catch::Matchers::WithinAbs;
namespace ad = crd::hesap::autodiff::forward;
using cd     = std::complex<double>;

namespace
{
struct Square // z²   (holomorphic; f'=2z)
{
    template <class S>
    S operator()(const S& z) const { return z * z; }
};
struct ExpF // exp(z) (holomorphic; f'=exp z). ADL: forward::exp for CDual (crd::math inside), std::exp for the oracle.
{
    template <class S>
    S operator()(const S& z) const { return exp(z); }
};
struct InvF // 1/z    (holomorphic; f'=−1/z²)
{
    template <class S>
    S operator()(const S& z) const { return cd(1.0, 0.0) / z; }
};
struct ConjF // conj(z) (non-holomorphic; ∂/∂z=0, ∂/∂z̄=1). ADL: forward::conj for CDual, std::conj for the oracle.
{
    template <class S>
    S operator()(const S& z) const { return conj(z); }
};
struct AbsF // |z| (non-holomorphic; ∂/∂z=z̄/(2|z|))
{
    template <class S>
    S operator()(const S& z) const { return ad::abs(z); }
};
struct NormF // |z|² (non-holomorphic; ∂/∂z=z̄, ∂/∂z̄=z)
{
    template <class S>
    S operator()(const S& z) const { return ad::norm(z); }
};

// helper: FD reconstruction of the Wirtinger pair via a 2×2 real Jacobian (complex-step cannot validate these).
template <class F>
ad::Wirtinger<double> wirtinger_fd(const F& f, cd z, double h)
{
    const cd fx = (f(z + cd(h, 0.0)) - f(z - cd(h, 0.0))) / (2.0 * h);   // ∂f/∂x
    const cd fy = (f(z + cd(0.0, h)) - f(z - cd(0.0, h))) / (2.0 * h);   // ∂f/∂y
    const cd im(0.0, 1.0);
    return ad::Wirtinger<double>{(fx - im * fy) * 0.5, (fx + im * fy) * 0.5};
}
} // namespace

TEST_CASE("holomorphic ops: complex derivative via real-dual code + Cauchy-Riemann", "[autodiff][complex]")
{
    const cd z(1.5, 0.7);

    // z²: derivative 2z, ∂/∂z̄=0
    {
        const auto w = ad::wirtinger<double>(Square{}, z);
        CHECK_THAT(w.dz.real(), WithinAbs((2.0 * z).real(), 1e-12));
        CHECK_THAT(w.dz.imag(), WithinAbs((2.0 * z).imag(), 1e-12));
        CHECK(ad::holomorphy_defect<double>(Square{}, z) < 1e-12); // CR holds
    }
    // exp(z): derivative exp(z)
    {
        const auto w   = ad::wirtinger<double>(ExpF{}, z);
        const cd   ref = crd::math::exp(z);
        CHECK_THAT(w.dz.real(), WithinAbs(ref.real(), 1e-11));
        CHECK_THAT(w.dz.imag(), WithinAbs(ref.imag(), 1e-11));
        CHECK(ad::holomorphy_defect<double>(ExpF{}, z) < 1e-11);
    }
    // 1/z: derivative −1/z²
    {
        const auto w   = ad::wirtinger<double>(InvF{}, z);
        const cd   ref = -cd(1.0, 0.0) / (z * z);
        CHECK_THAT(w.dz.real(), WithinAbs(ref.real(), 1e-12));
        CHECK_THAT(w.dz.imag(), WithinAbs(ref.imag(), 1e-12));
        CHECK(ad::holomorphy_defect<double>(InvF{}, z) < 1e-12);
    }
}

TEST_CASE("non-holomorphic ops: correct Wirtinger pair + FAIL Cauchy-Riemann", "[autodiff][complex]")
{
    const cd z(1.5, 0.7);

    // conj: ∂/∂z=0, ∂/∂z̄=1 — and NOT holomorphic
    {
        const auto w = ad::wirtinger<double>(ConjF{}, z);
        CHECK_THAT(w.dz.real(), WithinAbs(0.0, 1e-12));
        CHECK_THAT(w.dz.imag(), WithinAbs(0.0, 1e-12));
        CHECK_THAT(w.dzbar.real(), WithinAbs(1.0, 1e-12));
        CHECK_THAT(w.dzbar.imag(), WithinAbs(0.0, 1e-12));
        CHECK(ad::holomorphy_defect<double>(ConjF{}, z) > 0.5); // correctly non-holomorphic
    }
    // |z|² via the norm() pushforward: ∂/∂z=z̄, ∂/∂z̄=z
    {
        const auto w = ad::wirtinger<double>(NormF{}, z);
        CHECK_THAT(w.dz.real(), WithinAbs(std::conj(z).real(), 1e-12));
        CHECK_THAT(w.dz.imag(), WithinAbs(std::conj(z).imag(), 1e-12));
        CHECK_THAT(w.dzbar.real(), WithinAbs(z.real(), 1e-12));
        CHECK_THAT(w.dzbar.imag(), WithinAbs(z.imag(), 1e-12));
    }
    // |z| via abs(): ∂/∂z=z̄/(2|z|)
    {
        const auto w   = ad::wirtinger<double>(AbsF{}, z);
        const cd   ref = std::conj(z) / (2.0 * std::abs(z));
        CHECK_THAT(w.dz.real(), WithinAbs(ref.real(), 1e-12));
        CHECK_THAT(w.dz.imag(), WithinAbs(ref.imag(), 1e-12));
        CHECK(ad::holomorphy_defect<double>(AbsF{}, z) > 0.4); // |∂/∂z̄| = 1/2
    }
}

TEST_CASE("Wirtinger pair == 2x2 real-Jacobian finite difference", "[autodiff][complex]")
{
    const cd     z(1.5, 0.7);
    const double h = 1e-5;
    // holomorphic (exp) and non-holomorphic (conj) both validated against real FD
    {
        const auto wa = ad::wirtinger<double>(ExpF{}, z);
        const auto wf = wirtinger_fd(ExpF{}, z, h);
        CHECK_THAT(wa.dz.real(), WithinAbs(wf.dz.real(), 1e-6));
        CHECK_THAT(wa.dzbar.real(), WithinAbs(wf.dzbar.real(), 1e-6));
        CHECK_THAT(wa.dzbar.imag(), WithinAbs(wf.dzbar.imag(), 1e-6));
    }
    {
        const auto wa = ad::wirtinger<double>(ConjF{}, z);
        const auto wf = wirtinger_fd(ConjF{}, z, h);
        CHECK_THAT(wa.dz.real(), WithinAbs(wf.dz.real(), 1e-6));
        CHECK_THAT(wa.dzbar.real(), WithinAbs(wf.dzbar.real(), 1e-6));
    }
}

TEST_CASE("DFT is holomorphic: exact input sensitivity", "[autodiff][complex]")
{
    // Y_k = Σ_j x_j e^{−2πi kj/n}; linear ⇒ holomorphic. Seed x_0's tangent, read ∂Y_k/∂x_0 = e^{−2πi·0} = 1;
    // seed x_1's tangent, read ∂Y_2/∂x_1 = e^{−2πi·2/n}.
    constexpr int n = 4;
    ad::CDual<double> x[n];
    for (int j = 0; j < n; ++j) { x[j] = ad::CDual<double>{cd(0.3 * j - 0.5, 0.2 * j), cd(0.0, 0.0)}; }
    x[1].d = cd(1.0, 0.0); // seed ∂/∂x_1
    const double twopi = -6.283185307179586476925286766559;
    for (int k = 0; k < n; ++k)
    {
        ad::CDual<double> yk{cd(0.0, 0.0), cd(0.0, 0.0)};
        for (int j = 0; j < n; ++j)
        {
            const double ang = twopi * static_cast<double>((k * j) % n) / static_cast<double>(n);
            yk = yk + cd(std::cos(ang), std::sin(ang)) * x[j];
        }
        const cd expect(std::cos(twopi * static_cast<double>((k * 1) % n) / n),
                        std::sin(twopi * static_cast<double>((k * 1) % n) / n));
        CHECK_THAT(yk.d.real(), WithinAbs(expect.real(), 1e-12)); // ∂Y_k/∂x_1 = e^{−2πi k/n}
        CHECK_THAT(yk.d.imag(), WithinAbs(expect.imag(), 1e-12));
    }
}
