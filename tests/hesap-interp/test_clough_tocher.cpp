// crd-hesap-interp v13-f — Clough-Tocher C¹ scattered interpolation gated vs scipy CloughTocher2DInterpolator
// (bit-close ≤1e-9 — the macro-element + curvature-min gradient are transcribed from scipy _interpnd.pyx), the
// interpolation property, exact linear reproduction, C¹ smoothness, outside-hull NaN, + determinism.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/hesap/interp/interp.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd::hesap::interp;
using crd::usize;
using crd::containers::ConstSpan;
using crd::math::Vec2;

namespace
{
[[nodiscard]] bool close(double a, double b, double tol)
{
    const double d = a < b ? b - a : a - b;
    return d <= tol + tol * (b < 0 ? -b : b);
}

// Gate dataset + scipy references (tol=1e-10, maxiter=10000) — build/ct_gate_ref.py.
constexpr crd::usize kN = 16;
constexpr crd::usize kNq = 8;
constexpr double kPts[] = {0,
                           0,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           0.39963209507788999,
                           0.86057144512793293,
                           0.68559515344912403,
                           0.57892678735762926,
                           0.22481491235394924,
                           0.22479561626896213,
                           0.14646688973455957,
                           0.79294091661994814,
                           0.58089200939456709,
                           0.66645806223683635,
                           0.11646759543664197,
                           0.87592788172959546,
                           0.76595411264033741,
                           0.26987128854262094,
                           0.24545997376568052,
                           0.24672360788274705,
                           0.34339379436763018,
                           0.51980514530579025,
                           0.44555601491369268,
                           0.33298331215843358,
                           0.58948231577790355,
                           0.21159508852163347,
                           0.33371571882817452,
                           0.39308947463495336};
constexpr double kValQ[] = {1,
                            4,
                            0.5,
                            2.5,
                            1.124778192948297,
                            2.0329729469193971,
                            1.2501050810334626,
                            0.71968347146213385,
                            1.6677044945410298,
                            0.65217962276517261,
                            2.6784288722557603,
                            1.2743224373947291,
                            1.2415025748044826,
                            1.6637251055846491,
                            2.2125136216033292,
                            1.3317876749571997};
constexpr double kValL[] = {1.5,
                            3.5,
                            -1.5,
                            0.5,
                            -0.28245014522801881,
                            1.1344099448253604,
                            1.275242975901012,
                            -0.58588897039072529,
                            0.66240983207862514,
                            -0.89484845431550264,
                            2.2222943596528122,
                            1.2507491238831199,
                            0.62737215281788972,
                            1.3921620933520846,
                            2.0441793659909067,
                            0.98816301375148896};
constexpr double kQ[] = {0.4, 0.4, 0.6, 0.5, 0.5, 0.6, 0.35, 0.45, 0.3, 0.3, 0.65, 0.4, 0.45, 0.7, 0.55, 0.55};
constexpr double kRefQ[] = {1.4816966340616453, 1.8869005713552072, 1.5298999536858966, 1.3165471610843833,
                            1.3457445895622724, 2.1405823493246436, 1.3333008959677195, 1.7020024651624139};
constexpr double kRefL[] = {1.0999999999999999,  1.2,
                            0.70000000000000018, 0.84999999999999987,
                            1.2000000000000002,  1.5999999999999999,
                            0.30000000000000027, 0.94999999999999996};

void fill_pts(Vec2<double>* out)
{
    for (usize i = 0; i < kN; ++i)
    {
        out[i] = Vec2<double>{kPts[2 * i], kPts[2 * i + 1]};
    }
}
} // namespace

TEST_CASE("v13-f: Clough-Tocher bit-close vs scipy CloughTocher2DInterpolator", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    Vec2<double> vpts[kN];
    fill_pts(vpts);
    const auto pts = ConstSpan<Vec2<double>>{vpts, kN};

    CloughTocher2DInterpolant<double> ct(&alloc);
    REQUIRE(ct.fit(pts, ConstSpan<double>{kValQ, kN}, 1e-10, 10000) == InterpStatus::Ok);
    REQUIRE(ct.triangle_count() > 0);

    for (usize i = 0; i < kNq; ++i)
    {
        const double v = ct.eval(Vec2<double>{kQ[2 * i], kQ[2 * i + 1]});
        CHECK(close(v, kRefQ[i], 1e-9)); // bit-close vs scipy
    }
}

TEST_CASE("v13-f: Clough-Tocher interpolation property + outside-hull NaN", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    Vec2<double> vpts[kN];
    fill_pts(vpts);
    CloughTocher2DInterpolant<double> ct(&alloc);
    REQUIRE(ct.fit(ConstSpan<Vec2<double>>{vpts, kN}, ConstSpan<double>{kValQ, kN}, 1e-10, 10000) == InterpStatus::Ok);

    // interpolation property: the surface passes through every data site exactly.
    for (usize i = 0; i < kN; ++i)
    {
        CHECK(close(ct.eval(vpts[i]), kValQ[i], 1e-9));
    }

    // outside the convex hull → NaN (scipy fill_value default).
    CHECK(std::isnan(ct.eval(Vec2<double>{-0.5, -0.5})));
    CHECK(std::isnan(ct.eval(Vec2<double>{2.0, 0.5})));
    CHECK(std::isnan(ct.eval(Vec2<double>{0.5, 1.5})));
}

TEST_CASE("v13-f: Clough-Tocher reproduces a linear field exactly", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    Vec2<double> vpts[kN];
    fill_pts(vpts);
    CloughTocher2DInterpolant<double> ct(&alloc);
    REQUIRE(ct.fit(ConstSpan<Vec2<double>>{vpts, kN}, ConstSpan<double>{kValL, kN}, 1e-10, 10000) == InterpStatus::Ok);

    for (usize i = 0; i < kNq; ++i)
    {
        const double v = ct.eval(Vec2<double>{kQ[2 * i], kQ[2 * i + 1]});
        CHECK(close(v, kRefL[i], 1e-9)); // = 2x − 3y + 1.5 exactly
    }
}

TEST_CASE("v13-f: Clough-Tocher is C1-smooth (no derivative jumps across edges)", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    Vec2<double> vpts[kN];
    fill_pts(vpts);
    CloughTocher2DInterpolant<double> ct(&alloc);
    REQUIRE(ct.fit(ConstSpan<Vec2<double>>{vpts, kN}, ConstSpan<double>{kValQ, kN}, 1e-10, 10000) == InterpStatus::Ok);

    // Sample along an interior line; the finite-difference first derivative must vary SMOOTHLY (a C0-only interpolant
    // would jump by O(1) at every triangle edge it crosses). Quadratic field ⇒ a true C1 surface has |Δderiv| = O(h).
    constexpr int k_steps = 240;
    constexpr double x0 = 0.12;
    constexpr double x1 = 0.88;
    constexpr double yline = 0.46;
    const double h = (x1 - x0) / k_steps;
    double prev_d = 0.0;
    double max_jump = 0.0;
    bool have_prev = false;
    for (int i = 1; i < k_steps; ++i)
    {
        const double x = x0 + i * h;
        const double fm = ct.eval(Vec2<double>{x - h, yline});
        const double fp = ct.eval(Vec2<double>{x + h, yline});
        if (std::isnan(fm) || std::isnan(fp))
        {
            continue;
        }
        const double d = (fp - fm) / (2.0 * h);
        if (have_prev)
        {
            const double jump = std::abs(d - prev_d);
            max_jump = jump > max_jump ? jump : max_jump;
        }
        prev_d = d;
        have_prev = true;
    }
    // C1 sanity check (the rigorous C1 proof is the bit-close scipy match above — scipy's CT is provably C1). The
    // consecutive-FD-jump ≈ f″·h is bounded for any C1 surface (the cubic has locally high curvature near an edge);
    // a C0 regression (linear/NNI) would jump by O(0.5) at every edge crossing. The CT measures ~0.05 here.
    CHECK(max_jump < 0.15);
}

TEST_CASE("v13-f: Clough-Tocher fit is deterministic + rejects bad input", "[v13-f][interp]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    Vec2<double> vpts[kN];
    fill_pts(vpts);
    const auto pts = ConstSpan<Vec2<double>>{vpts, kN};
    CloughTocher2DInterpolant<double> a(&alloc);
    CloughTocher2DInterpolant<double> b(&alloc);
    REQUIRE(a.fit(pts, ConstSpan<double>{kValQ, kN}, 1e-10, 10000) == InterpStatus::Ok);
    REQUIRE(b.fit(pts, ConstSpan<double>{kValQ, kN}, 1e-10, 10000) == InterpStatus::Ok);
    for (usize i = 0; i < kNq; ++i)
    {
        const Vec2<double> qv{kQ[2 * i], kQ[2 * i + 1]};
        CHECK(a.eval(qv) == b.eval(qv)); // bit-identical
    }

    CloughTocher2DInterpolant<double> bad(&alloc);
    Vec2<double> two[2] = {{0, 0}, {1, 1}};
    CHECK(bad.fit(ConstSpan<Vec2<double>>{two, 2}, ConstSpan<double>{kValQ, 2}) == InterpStatus::BadInput);
}

TEST_CASE("v13-f: Clough-Tocher locates every interior point (no NaN) + eval is order-independent", "[v13-f][interp]")
{
    // hull = [0,1] (the 4 corners). A dense interior grid must (a) NEVER return NaN — the fast-walk locate's exact
    // linear-scan fallback guarantees no interior query is spuriously dropped — and (b) be bit-identical regardless
    // of evaluation order (the m_last_tri walk-start cache must not change the located triangle ⇒ eval is a pure
    // function of the query, the property a safety-critical replay depends on).
    crd::memory::TlsfAllocator alloc(1U << 22);
    Vec2<double> vpts[kN];
    fill_pts(vpts);
    CloughTocher2DInterpolant<double> ct(&alloc);
    REQUIRE(ct.fit(ConstSpan<Vec2<double>>{vpts, kN}, ConstSpan<double>{kValQ, kN}, 1e-10, 10000) == InterpStatus::Ok);

    constexpr int k_g = 200;
    crd::containers::Array<double> fwd(&alloc);
    fwd.resize(static_cast<crd::usize>(k_g) * k_g);
    auto at = [&](int ix, int iy)
    {
        const double x = 0.005 + 0.99 * (static_cast<double>(ix) / (k_g - 1));
        const double y = 0.005 + 0.99 * (static_cast<double>(iy) / (k_g - 1));
        return ct.eval(Vec2<double>{x, y});
    };
    bool any_nan = false;
    for (int iy = 0; iy < k_g; ++iy)
    {
        for (int ix = 0; ix < k_g; ++ix)
        {
            const double v = at(ix, iy);
            fwd[static_cast<usize>(iy) * k_g + ix] = v;
            any_nan = any_nan || std::isnan(v);
        }
    }
    CHECK_FALSE(any_nan); // (a) no interior query NaNs
    bool order_independent = true;
    for (int iy = k_g - 1; iy >= 0; --iy)
    {
        for (int ix = k_g - 1; ix >= 0; --ix)
        {
            order_independent = order_independent && (at(ix, iy) == fwd[static_cast<usize>(iy) * k_g + ix]);
        }
    }
    CHECK(order_independent); // (b) reverse-order eval is bit-identical
}
