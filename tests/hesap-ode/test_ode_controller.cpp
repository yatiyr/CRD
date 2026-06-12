// v9-a gates: the step-control substrate — WRMS norm exact hand-computed values, the scipy-exact
// elementary controller semantics (accept strictly-below-1, post-rejection growth cap, clamps), the
// Hairer-form PI controller (accept-at-1, facold floor, never-grow-on-reject), and the cubic-Hermite
// dense-output fallback (exact on cubics, exact endpoints).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/controller.hpp>
#include <crd/hesap/ode/dense_output.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

TEST_CASE("error_norm_wrms: exact hand-computed values, vector atol, n = 0", "[ode][controller]")
{
    const f64 e[2] = {0.001, -0.002};
    const f64 y0[2] = {1.0, -4.0};
    const f64 y1[2] = {2.0, -3.0};
    const f64 rtol = 0.1;
    const f64 atol = 0.5;

    // scale = {0.5 + 0.1*2, 0.5 + 0.1*4} = {0.7, 0.9}; err = sqrt(((1e-3/0.7)^2 + (2e-3/0.9)^2)/2)
    const f64 q0 = 0.001 / 0.7;
    const f64 q1 = 0.002 / 0.9;
    const f64 expected = std::sqrt((q0 * q0 + q1 * q1) / 2.0);
    const f64 norm = ode::error_norm_wrms<f64>(containers::ConstSpan<f64>(e, 2), containers::ConstSpan<f64>(y0, 2),
                                               containers::ConstSpan<f64>(y1, 2), rtol, atol);
    CHECK(norm == Catch::Approx(expected).epsilon(1e-15));

    // Per-component atol overrides the scalar.
    const f64 atol_vec[2] = {0.5, 1.1};
    const f64 q1v = 0.002 / (1.1 + 0.4);
    const f64 expected_vec = std::sqrt((q0 * q0 + q1v * q1v) / 2.0);
    const f64 norm_vec =
        ode::error_norm_wrms<f64>(containers::ConstSpan<f64>(e, 2), containers::ConstSpan<f64>(y0, 2),
                                  containers::ConstSpan<f64>(y1, 2), rtol, atol, containers::ConstSpan<f64>(atol_vec, 2));
    CHECK(norm_vec == Catch::Approx(expected_vec).epsilon(1e-15));

    // n == 0 is a defined no-op.
    CHECK(ode::error_norm_wrms<f64>({}, {}, {}, rtol, atol) == 0.0);
}

TEST_CASE("ElementaryController: scipy-exact accept/reject semantics and clamps", "[ode][controller]")
{
    ode::ElementaryController<f64> c; // exponent -0.2 (q = 4), scipy constants

    bool accept = false;

    // err == 0 => max_factor.
    CHECK(c.update(0.0, accept) == 10.0);
    CHECK(accept);

    // Plain accept: factor = safety * err^(-1/5), unclamped region.
    const f64 err = 0.5;
    const f64 expected = 0.9 * std::pow(err, -0.2);
    CHECK(c.update(err, accept) == Catch::Approx(expected).epsilon(1e-15));
    CHECK(accept);

    // Tiny error clamps at max_factor.
    CHECK(c.update(1e-12, accept) == 10.0);
    CHECK(accept);

    // err == 1.0 REJECTS (scipy: accept iff err < 1, strictly).
    const f64 reject_factor = c.update(1.0, accept);
    CHECK_FALSE(accept);
    CHECK(reject_factor == Catch::Approx(0.9).epsilon(1e-15)); // 0.9 * 1^(-0.2)

    // Immediately after a rejection, an accepted step's growth is capped at 1.
    const f64 post_reject = c.update(0.5, accept);
    CHECK(accept);
    CHECK(post_reject == 1.0);

    // ... and the cap lifts once the rejection history clears.
    CHECK(c.update(0.5, accept) == Catch::Approx(expected).epsilon(1e-15));

    // Huge error clamps at min_factor on rejection.
    CHECK(c.update(1e12, accept) == 0.2);
    CHECK_FALSE(accept);
}

TEST_CASE("PiController: Hairer-form factor, facold floor, reject never grows", "[ode][controller]")
{
    ode::PiController<f64> c; // beta1 0.17, beta2 0.04, err_prev 1, floor 1e-4

    bool accept = false;

    // First accept: factor = 0.9 * err^(-0.17) * 1^(0.04).
    const f64 err1 = 0.25;
    const f64 f1 = c.update(err1, accept);
    CHECK(accept);
    CHECK(f1 == Catch::Approx(0.9 * std::pow(err1, -0.17)).epsilon(1e-15));

    // History now feeds in: factor = 0.9 * err^(-0.17) * err1^(0.04).
    const f64 err2 = 0.5;
    const f64 f2 = c.update(err2, accept);
    CHECK(accept);
    CHECK(f2 == Catch::Approx(0.9 * std::pow(err2, -0.17) * std::pow(err1, 0.04)).epsilon(1e-15));

    // err == 1.0 ACCEPTS (Hairer convention, unlike scipy).
    (void)c.update(1.0, accept);
    CHECK(accept);

    // err below the floor uses the floor (also guards err = 0).
    ode::PiController<f64> c2;
    const f64 f_floor = c2.update(0.0, accept);
    CHECK(accept);
    const f64 expected_floor = 0.9 * std::pow(1e-4, -0.17);
    const f64 clamped = expected_floor < 10.0 ? expected_floor : 10.0;
    CHECK(f_floor == Catch::Approx(clamped).epsilon(1e-15));

    // A rejection never returns a factor > 1.
    ode::PiController<f64> c3;
    const f64 f_rej = c3.update(4.0, accept);
    CHECK_FALSE(accept);
    CHECK(f_rej <= 1.0);

    // Determinism: an identical controller fed the identical sequence produces identical factors.
    ode::PiController<f64> a1;
    ode::PiController<f64> a2;
    const f64 seq[5] = {0.3, 1.7, 0.8, 0.05, 0.99};
    for (const f64 e : seq)
    {
        bool acc1 = false;
        bool acc2 = false;
        const f64 v1 = a1.update(e, acc1);
        const f64 v2 = a2.update(e, acc2);
        CHECK(v1 == v2);
        CHECK(acc1 == acc2);
    }
}

TEST_CASE("hermite_eval: exact on cubics, exact endpoints, vector state", "[ode][dense]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // Two cubic components: p(t) = 2t^3 - t^2 + 3t - 5 and q(t) = -t^3 + 4t^2 + 1 on [t0, t1] = [0.5, 1.7].
    auto p = [](f64 t) { return 2.0 * t * t * t - t * t + 3.0 * t - 5.0; };
    auto dp = [](f64 t) { return 6.0 * t * t - 2.0 * t + 3.0; };
    auto q = [](f64 t) { return -t * t * t + 4.0 * t * t + 1.0; };
    auto dq = [](f64 t) { return -3.0 * t * t + 8.0 * t; };

    const f64 t0 = 0.5;
    const f64 t1 = 1.7;
    const f64 y0[2] = {p(t0), q(t0)};
    const f64 f0[2] = {dp(t0), dq(t0)};
    const f64 y1[2] = {p(t1), q(t1)};
    const f64 f1[2] = {dp(t1), dq(t1)};

    containers::Array<f64> out(&alloc);
    out.resize(2);

    // Interior points: a cubic interpolant with cubic data is exact (to roundoff).
    const f64 samples[5] = {0.5, 0.8, 1.1, 1.4, 1.7};
    for (const f64 t : samples)
    {
        ode::hermite_eval<f64>(t0, t1, containers::ConstSpan<f64>(y0, 2), containers::ConstSpan<f64>(f0, 2),
                               containers::ConstSpan<f64>(y1, 2), containers::ConstSpan<f64>(f1, 2), t,
                               containers::Span<f64>(out.data(), 2));
        CHECK(out[0] == Catch::Approx(p(t)).epsilon(1e-13));
        CHECK(out[1] == Catch::Approx(q(t)).epsilon(1e-13));
    }

    // theta = 0 reproduces y0 EXACTLY (h00 = 1, the rest 0 — bit-level).
    ode::hermite_eval<f64>(t0, t1, containers::ConstSpan<f64>(y0, 2), containers::ConstSpan<f64>(f0, 2),
                           containers::ConstSpan<f64>(y1, 2), containers::ConstSpan<f64>(f1, 2), t0,
                           containers::Span<f64>(out.data(), 2));
    CHECK(out[0] == y0[0]);
    CHECK(out[1] == y0[1]);
}
