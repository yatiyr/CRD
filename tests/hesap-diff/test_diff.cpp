// crd-hesap-diff v13-l/m - numerical differentiation: Fornberg weights / central+Ridders FD / complex-step
// (machine-exact vs JAX autodiff) / Savitzky-Golay (vs scipy) / Chebyshev+Fourier spectral diff. Gated vs analytic
// derivatives + scipy + determinism.

#include <crd/hesap/diff/diff.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>

namespace d = crd::hesap::diff;
using crd::f64;
using crd::containers::ConstSpan;
using crd::containers::Span;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
} // namespace

TEST_CASE("v13-l: Fornberg weights reproduce the analytic FD stencils", "[v13-l][diff]")
{
    const f64 x3[3] = {-1.0, 0.0, 1.0};
    f64 c[3 * 3]; // (max_deriv+1) * nnodes = 3*3
    d::fornberg_weights<f64>(0.0, ConstSpan<f64>{x3, 3}, 2, c);
    // 1st derivative central: [-1/2, 0, 1/2]
    CHECK(close(c[1 * 3 + 0], -0.5, 1e-14, 1e-15));
    CHECK(close(c[1 * 3 + 2], 0.5, 1e-14, 1e-15));
    // 2nd derivative central: [1, -2, 1]
    CHECK(close(c[2 * 3 + 0], 1.0, 1e-14, 1e-15));
    CHECK(close(c[2 * 3 + 1], -2.0, 1e-14, 1e-15));
    // 5-point 1st derivative: [1/12, -2/3, 0, 2/3, -1/12]
    const f64 x5[5] = {-2.0, -1.0, 0.0, 1.0, 2.0};
    f64 c5[2 * 5];
    d::fornberg_weights<f64>(0.0, ConstSpan<f64>{x5, 5}, 1, c5);
    CHECK(close(c5[1 * 5 + 0], 1.0 / 12.0, 1e-14, 1e-15));
    CHECK(close(c5[1 * 5 + 1], -2.0 / 3.0, 1e-14, 1e-15));
}

TEST_CASE("v13-m: complex-step is MACHINE-EXACT (matches analytic to ~1e-15)", "[v13-m][diff]")
{
    // f = exp(x) sin(x); f'(x) = exp(x)(sin x + cos x)
    auto f1 = [](std::complex<f64> z)
    {
        return std::exp(z) * std::sin(z);
    };
    const f64 x = 1.3;
    const f64 got = d::derivative_complex_step<f64>(f1, x);
    const f64 ref = std::exp(x) * (std::sin(x) + std::cos(x));
    CHECK(close(got, ref, 1e-14, 1e-14)); // machine-exact, no cancellation
    // f = tanh(2x); f' = 2 sech^2(2x) = 2(1 - tanh^2(2x))
    auto f2 = [](std::complex<f64> z)
    {
        return std::tanh(2.0 * z);
    };
    const f64 g2 = d::derivative_complex_step<f64>(f2, 0.4);
    const f64 r2 = 2.0 * (1.0 - std::tanh(0.8) * std::tanh(0.8));
    CHECK(close(g2, r2, 1e-14, 1e-14));
    // determinism
    CHECK(d::derivative_complex_step<f64>(f1, x) == d::derivative_complex_step<f64>(f1, x));
}

TEST_CASE("v13-m: complex-step gradient of a scalar field", "[v13-m][diff]")
{
    // f(x,y,z) = exp(x*y) + sin(z); grad = (y e^{xy}, x e^{xy}, cos z)
    auto f = [](ConstSpan<std::complex<f64>> v)
    {
        return std::exp(v[0] * v[1]) + std::sin(v[2]);
    };
    std::complex<f64> xv[3] = {{0.5, 0.0}, {1.2, 0.0}, {0.3, 0.0}};
    f64 g[3];
    d::gradient_complex_step<f64>(f, Span<std::complex<f64>>{xv, 3}, g);
    const f64 e = std::exp(0.5 * 1.2);
    CHECK(close(g[0], 1.2 * e, 1e-13, 1e-14));
    CHECK(close(g[1], 0.5 * e, 1e-13, 1e-14));
    CHECK(close(g[2], std::cos(0.3), 1e-13, 1e-14));
}

TEST_CASE("v13-l: central FD + Ridders extrapolation vs analytic", "[v13-l][diff]")
{
    // f = exp(sin x); f' = cos x exp(sin x) @ x=1
    auto f = [](f64 x)
    {
        return std::exp(std::sin(x));
    };
    const f64 ref = std::cos(1.0) * std::exp(std::sin(1.0));
    // 4th-order central with optimal step
    CHECK(close(d::derivative_central<f64>(f, 1.0), ref, 1e-9, 1e-10));
    // Ridders: high accuracy + an error estimate
    const auto r = d::derivative_ridders<f64>(f, 1.0);
    CHECK(close(r.value, ref, 1e-12, 1e-13));
    CHECK(r.error_estimate < 1e-9);
    // 2nd derivative: f'' = (cos^2 x - sin x) exp(sin x)
    const f64 r2 = (std::cos(1.0) * std::cos(1.0) - std::sin(1.0)) * std::exp(std::sin(1.0));
    CHECK(close(d::second_derivative_central<f64>(f, 1.0), r2, 1e-6, 1e-7));
}

TEST_CASE("v13-l: FD Jacobian + Hessian-vector + forward difference", "[v13-l][diff]")
{
    // forward difference: f=exp, f'(1)=e (one-sided, ~sqrt(eps))
    CHECK(close(d::derivative_forward<f64>([](f64 x) { return std::exp(x); }, 1.0), std::exp(1.0), 1e-7, 1e-8));
    // FD Jacobian: f(x,y) = [x^2 y, x + sin y]; J = [[2xy, x^2],[1, cos y]] at (1.5, 0.8)
    auto fvec = [](ConstSpan<f64> in, Span<f64> out)
    {
        out[0] = in[0] * in[0] * in[1];
        out[1] = in[0] + std::sin(in[1]);
    };
    f64 xv[2] = {1.5, 0.8};
    f64 fp[2];
    f64 fm[2];
    f64 jac[4];
    d::jacobian_central<f64>(fvec, Span<f64>{xv, 2}, Span<f64>{fp, 2}, Span<f64>{fm, 2}, jac);
    CHECK(close(jac[0], 2.0 * 1.5 * 0.8, 1e-7, 1e-8)); // dF0/dx = 2xy
    CHECK(close(jac[1], 1.5 * 1.5, 1e-7, 1e-8));       // dF0/dy = x^2
    CHECK(close(jac[2], 1.0, 1e-7, 1e-8));             // dF1/dx = 1
    CHECK(close(jac[3], std::cos(0.8), 1e-7, 1e-8));   // dF1/dy = cos y
    // Hessian-vector: f = 0.5 (3 x^2 + 2 y^2) -> H = diag(3,2); H*v with v=(1,1) is (3,2)
    auto fq = [](ConstSpan<f64> v)
    {
        return 0.5 * (3.0 * v[0] * v[0] + 2.0 * v[1] * v[1]);
    };
    f64 xq[2] = {0.4, -0.6};
    f64 vq[2] = {1.0, 1.0};
    f64 gp[2];
    f64 gm[2];
    f64 hv[2];
    d::hessian_vector_central<f64>(fq, Span<f64>{xq, 2}, ConstSpan<f64>{vq, 2}, gp, gm, hv);
    CHECK(close(hv[0], 3.0, 1e-5, 1e-6));
    CHECK(close(hv[1], 2.0, 1e-5, 1e-6));
}

TEST_CASE("v13-m: Savitzky-Golay coefficients match scipy.signal.savgol_coeffs", "[v13-m][diff]")
{
    f64 c5[5];
    REQUIRE(d::savgol_coeffs<f64>(5, 2, 1, 1.0, c5)); // window=5, poly=2, deriv=1
    const f64 ref5[5] = {-0.2, -0.1, 0.0, 0.1, 0.2};
    for (int i = 0; i < 5; ++i)
    {
        CHECK(close(c5[i], ref5[i], 1e-12, 1e-12));
    }
    f64 c7[7];
    REQUIRE(d::savgol_coeffs<f64>(7, 3, 1, 1.0, c7)); // window=7, poly=3, deriv=1
    const f64 ref7[7] = {0.087301587301587, -0.265873015873016, -0.230158730158730, 0.0,
                         0.230158730158730, 0.265873015873016,  -0.087301587301587};
    for (int i = 0; i < 7; ++i)
    {
        CHECK(close(c7[i], ref7[i], 1e-10, 1e-11));
    }
    // application: differentiate a quadratic exactly. y=x^2 sampled at dx=1, deriv=1 -> 2x (interior exact)
    f64 y[9];
    for (int i = 0; i < 9; ++i)
    {
        const f64 xi = static_cast<f64>(i);
        y[i] = xi * xi;
    }
    f64 out[9];
    REQUIRE(d::savgol_filter<f64>(ConstSpan<f64>{y, 9}, 5, 2, 1, 1.0, out));
    CHECK(close(out[4], 8.0, 1e-10, 1e-11)); // d/dx x^2 at x=4 is 8 (centre, full window)
    // bad input
    CHECK(!d::savgol_coeffs<f64>(4, 2, 1, 1.0, c5)); // even window
}

TEST_CASE("v13-m: Chebyshev + Fourier spectral differentiation (exponential accuracy)", "[v13-m][diff]")
{
    // Chebyshev: differentiate f = exp(x) on [-1,1]; f' = exp(x). Spectral accuracy at n=16.
    {
        constexpr int n = 16;
        f64 d[n * n];
        f64 nodes[n];
        d::chebyshev_diff_matrix<f64>(n, d, nodes);
        f64 u[n];
        for (int i = 0; i < n; ++i)
        {
            u[i] = std::exp(nodes[i]);
        }
        // (D u)_i ~ exp(nodes_i)
        f64 maxerr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            f64 du = 0.0;
            for (int j = 0; j < n; ++j)
            {
                du += d[i * n + j] * u[j];
            }
            maxerr = std::max(maxerr, std::abs(du - std::exp(nodes[i])));
        }
        CHECK(maxerr < 1e-10); // spectral
    }
    // Fourier: differentiate periodic f = sin(x) on [0,2pi); f' = cos(x).
    {
        constexpr int n = 24;
        f64 d[n * n];
        f64 nodes[n];
        d::fourier_diff_matrix<f64>(n, d, nodes);
        f64 u[n];
        for (int i = 0; i < n; ++i)
        {
            u[i] = std::sin(nodes[i]);
        }
        f64 maxerr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            f64 du = 0.0;
            for (int j = 0; j < n; ++j)
            {
                du += d[i * n + j] * u[j];
            }
            maxerr = std::max(maxerr, std::abs(du - std::cos(nodes[i])));
        }
        CHECK(maxerr < 1e-12); // spectral (band-limited -> exact)
    }
}
