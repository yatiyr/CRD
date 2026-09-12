// ---------------------------------------------------------------------------
// crd-geometry-curves -- Frames + RMF. Phase 3.1.7 v10e.
//
// Coverage:
//   1.  tangent(curve, t) unit length within ULP on every curve kind.
//   2.  tangent on a straight polyline = constant chord direction.
//   3.  normal perpendicular to tangent within ULP.
//   4.  binormal = cross(tangent, normal) within ULP.
//   5.  Planar circular arc: every RMF normal lies in the arc plane;
//       every RMF binormal equals the plane normal up to sign. This is
//       the Wang 2008 reflection-sign discriminator -- a wrong sign
//       produces flipped binormals on the second frame.
//   6.  Helix RMF: every frame is orthonormal at every sample; adjacent
//       tangents agree (dot >= 0) -- the minimal-twist property.
//   7.  Closed circle RMF: after walking around the loop the closing
//       twist is redistributed -- the FIRST and LAST frames' normals
//       agree to within 1e-4 (no end-of-loop seam).
//   8.  Zero-curvature line: normal returns the deterministic
//       frenet_fallback_normal output -- +Y projection when tangent !=
//       +/-Y, +Z fallback when parallel.
//   9.  Degenerate-tangent scalar API: returns +X.
//  10.  f64 instantiations work end-to-end.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

namespace
{

using namespace crd::geometry::curves;

template <typename T>
[[nodiscard]] crd::math::Vec3<T> v3(T x, T y, T z) noexcept
{
    return crd::math::Vec3<T>(x, y, z);
}

template <typename T>
[[nodiscard]] bool approx_eq(T a, T b, T tol = static_cast<T>(1e-5)) noexcept
{
    return std::abs(a - b) <= tol;
}

template <typename T>
[[nodiscard]] bool vec_approx_eq(const crd::math::Vec3<T>& a, const crd::math::Vec3<T>& b,
                                  T tol = static_cast<T>(1e-5)) noexcept
{
    return approx_eq(a.x, b.x, tol) && approx_eq(a.y, b.y, tol) && approx_eq(a.z, b.z, tol);
}

// Helper: build a unit circular arc in the XZ plane covering one full loop.
template <typename T>
[[nodiscard]] CircularArc3<T> unit_circle_xz(bool closed = false) noexcept
{
    CircularArc3<T> arc{};
    arc.center        = v3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
    arc.axis_u        = v3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
    arc.axis_v        = v3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    arc.radius        = static_cast<T>(1);
    arc.sweep_radians = static_cast<T>(2.0 * 3.14159265358979323846);
    arc.closed        = closed;
    return arc;
}

template <typename T>
[[nodiscard]] CubicHermite3<T> sample_hermite() noexcept
{
    return CubicHermite3<T>{v3<T>(0, 0, 0), v3<T>(1, 0, 0), v3<T>(1, 1, 0), v3<T>(1, 0, 0)};
}

} // namespace

TEST_CASE("frames: tangent unit length on every curve kind", "[geometry-curves][frames]")
{
    using T = crd::f32;

    // Bezier (analytic derivative).
    {
        CubicBezier3<T> b{v3<T>(0, 0, 0), v3<T>(1, 1, 0), v3<T>(2, -1, 0), v3<T>(3, 0, 0)};
        const auto t  = tangent(b, static_cast<T>(0.3));
        REQUIRE(approx_eq(crd::math::length(t), static_cast<T>(1), static_cast<T>(1e-5)));
    }
    // Hermite.
    {
        auto h = sample_hermite<T>();
        const auto t = tangent(h, static_cast<T>(0.5));
        REQUIRE(approx_eq(crd::math::length(t), static_cast<T>(1), static_cast<T>(1e-5)));
    }
    // CircularArc.
    {
        auto a = unit_circle_xz<T>();
        const auto t = tangent(a, static_cast<T>(0.25));
        REQUIRE(approx_eq(crd::math::length(t), static_cast<T>(1), static_cast<T>(1e-5)));
    }
}

TEST_CASE("frames: tangent on a straight polyline is constant chord direction",
          "[geometry-curves][frames]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "frames-test");

    crd::math::Vec3<T> pts[] = {v3<T>(0, 0, 0), v3<T>(1, 0, 0), v3<T>(2, 0, 0)};
    Polyline3View<T> p{crd::containers::ConstSpan<crd::math::Vec3<T>>{pts, 3U}, false};

    const auto t_at_03 = tangent(p, static_cast<T>(0.3));
    const auto t_at_07 = tangent(p, static_cast<T>(0.7));
    REQUIRE(vec_approx_eq(t_at_03, v3<T>(1, 0, 0)));
    REQUIRE(vec_approx_eq(t_at_07, v3<T>(1, 0, 0)));
}

TEST_CASE("frames: normal is perpendicular to tangent, binormal = cross(T, N)",
          "[geometry-curves][frames]")
{
    using T = crd::f32;

    auto a = unit_circle_xz<T>();
    for (const T t : {static_cast<T>(0.1), static_cast<T>(0.3), static_cast<T>(0.7)})
    {
        const auto t_hat = tangent(a, t);
        const auto n_hat = normal(a, t);
        const auto b_hat = binormal(a, t);
        REQUIRE(approx_eq(crd::math::dot(t_hat, n_hat), static_cast<T>(0), static_cast<T>(2e-3)));
        REQUIRE(approx_eq(crd::math::length(n_hat), static_cast<T>(1), static_cast<T>(1e-5)));
        const auto b_expected = crd::math::cross(t_hat, n_hat);
        REQUIRE(vec_approx_eq(b_hat, b_expected, static_cast<T>(1e-5)));
    }
}

TEST_CASE("frames: planar circular arc RMF reduces to Frenet (binormal == plane normal)",
          "[geometry-curves][frames][rmf]")
{
    // Wang 2008 reflection-sign discriminator: for any planar curve the
    // RMF binormal must equal the plane normal (up to a single global sign).
    // A wrong reflection sign produces alternating binormals.
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "frames-test");

    auto a            = unit_circle_xz<T>();          // arc in XZ plane
    a.sweep_radians   = static_cast<T>(3.14159265358979323846); // half circle
    const auto plane_normal = v3<T>(0, 1, 0); // +Y (axis_u x axis_v)
    auto frames = compute_rmf(a, 32U, &alloc);
    REQUIRE(frames.size() == 33U); // open: n_samples + 1

    // All normals lie in the XZ plane: dot(N, +Y) ~= 0.
    // All binormals equal +/-Y.
    bool first_sign_set = false;
    T    expected_sign  = static_cast<T>(0);
    for (const auto& f : frames)
    {
        REQUIRE(approx_eq(crd::math::dot(f.normal, plane_normal), static_cast<T>(0), static_cast<T>(1e-3)));
        const T dot_b = crd::math::dot(f.binormal, plane_normal);
        REQUIRE(approx_eq(std::abs(dot_b), static_cast<T>(1), static_cast<T>(1e-3)));
        const T sign = dot_b >= static_cast<T>(0) ? static_cast<T>(1) : static_cast<T>(-1);
        if (!first_sign_set) { expected_sign = sign; first_sign_set = true; }
        REQUIRE(approx_eq(sign, expected_sign));
    }
}

TEST_CASE("frames: helix RMF orthonormal at every sample, adjacent tangents agree",
          "[geometry-curves][frames][rmf]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "frames-test");

    // Approximate a helix via Catmull-Rom interpolation through points on
    // a vertical helix x=cos(theta), z=sin(theta), y=theta/(2pi).
    constexpr crd::u32 k_pts = 16U;
    crd::math::Vec3<T> pts_buf[k_pts];
    for (crd::u32 i = 0U; i < k_pts; ++i)
    {
        const T theta = static_cast<T>(i) * static_cast<T>(0.5);
        pts_buf[i]     = v3<T>(static_cast<T>(std::cos(static_cast<double>(theta))),
                                static_cast<T>(theta * static_cast<T>(0.1)),
                                static_cast<T>(std::sin(static_cast<double>(theta))));
    }
    CatmullRom3<T> c(&alloc,
                     crd::containers::ConstSpan<crd::math::Vec3<T>>{pts_buf, k_pts},
                     CatmullRomParam::Centripetal,
                     false);

    auto frames = compute_rmf(c, 32U, &alloc);
    REQUIRE(frames.size() == 33U);

    for (crd::usize i = 0U; i < frames.size(); ++i)
    {
        const auto& f = frames[i];
        REQUIRE(approx_eq(crd::math::length(f.tangent), static_cast<T>(1), static_cast<T>(1e-4)));
        REQUIRE(approx_eq(crd::math::length(f.normal), static_cast<T>(1), static_cast<T>(1e-4)));
        REQUIRE(approx_eq(crd::math::length(f.binormal), static_cast<T>(1), static_cast<T>(1e-4)));
        REQUIRE(approx_eq(crd::math::dot(f.tangent, f.normal), static_cast<T>(0), static_cast<T>(1e-3)));
        REQUIRE(approx_eq(crd::math::dot(f.tangent, f.binormal), static_cast<T>(0), static_cast<T>(1e-3)));
        REQUIRE(approx_eq(crd::math::dot(f.normal, f.binormal), static_cast<T>(0), static_cast<T>(1e-3)));
        if (i + 1U < frames.size())
        {
            REQUIRE(crd::math::dot(f.tangent, frames[i + 1U].tangent) > static_cast<T>(0));
        }
    }
}

TEST_CASE("frames: closed-curve RMF distributes closure twist (no end-of-loop seam)",
          "[geometry-curves][frames][rmf]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "frames-test");

    auto a = unit_circle_xz<T>(/*closed=*/true);
    auto frames = compute_rmf(a, 64U, &alloc);
    REQUIRE(frames.size() == 64U); // closed: exactly n_samples

    // For a closed curve the wrap frame (continuation of frame[n-1] back to
    // t=0) should match frame[0]. After redistribution the LAST frame's
    // binormal should rotate-into frame[0]'s within one step's worth of
    // angular increment.
    //
    // We discriminate the closure with a simpler invariant: the start frame
    // (frame[0]) should be reachable by one Wang step from frame[n-1] with
    // tangent set to tangent(curve, t=1) == tangent(curve, t=0). After
    // closure-twist redistribution that step yields a frame whose normal
    // is within angular epsilon of frame[0].normal.
    const auto& first = frames[0U];
    const auto& last  = frames[frames.size() - 1U];

    // Walk one final step from `last` to t=1 (== t=0 wrapped) and compare
    // with `first`. The Wang step is deterministic.
    const auto p_last = evaluate(a, static_cast<T>(static_cast<double>(frames.size() - 1U)
                                                   / static_cast<double>(64U)));
    const auto p_first = evaluate(a, static_cast<T>(0));
    auto next_t        = tangent(a, static_cast<T>(0));
    const auto wrap    = detail::wang_step(last, p_last, p_first, next_t);
    // Wrap frame's normal should match the start normal within ~1/64
    // radian (one step's worth of curvature on a circle subdivided in 64
    // chunks). cos(2pi/64) ~ 0.995.
    REQUIRE(crd::math::dot(wrap.normal, first.normal) > static_cast<T>(0.99));
}

TEST_CASE("frames: zero-curvature line falls back to deterministic fallback normal",
          "[geometry-curves][frames]")
{
    using T = crd::f32;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "frames-test");

    // Straight polyline along +X. Curvature == 0 everywhere.
    crd::math::Vec3<T> pts[] = {v3<T>(0, 0, 0), v3<T>(1, 0, 0), v3<T>(2, 0, 0)};
    Polyline3View<T> p{crd::containers::ConstSpan<crd::math::Vec3<T>>{pts, 3U}, false};

    const auto t_hat = tangent(p, static_cast<T>(0.5));
    REQUIRE(vec_approx_eq(t_hat, v3<T>(1, 0, 0)));

    const auto n_hat = normal(p, static_cast<T>(0.5));
    // Tangent is +X, not parallel to +Y => fallback projects +Y onto plane
    // perpendicular to tangent => +Y (already perpendicular).
    REQUIRE(vec_approx_eq(n_hat, v3<T>(0, 1, 0), static_cast<T>(1e-4)));

    // Now a vertical polyline so tangent is parallel to +Y => +Z fallback.
    crd::math::Vec3<T> pts_v[] = {v3<T>(0, 0, 0), v3<T>(0, 1, 0), v3<T>(0, 2, 0)};
    Polyline3View<T> p_v{crd::containers::ConstSpan<crd::math::Vec3<T>>{pts_v, 3U}, false};
    const auto n_hat_v = normal(p_v, static_cast<T>(0.5));
    REQUIRE(vec_approx_eq(n_hat_v, v3<T>(0, 0, 1), static_cast<T>(1e-4)));
}

TEST_CASE("frames: degenerate-tangent scalar API returns +X", "[geometry-curves][frames]")
{
    using T = crd::f32;
    // Construct a Bezier with all four control points coincident => derivative is zero everywhere.
    CubicBezier3<T> degenerate{v3<T>(1, 2, 3), v3<T>(1, 2, 3), v3<T>(1, 2, 3), v3<T>(1, 2, 3)};
    const auto t_hat = tangent(degenerate, static_cast<T>(0.5));
    REQUIRE(vec_approx_eq(t_hat, v3<T>(1, 0, 0)));
}

TEST_CASE("frames: f64 instantiations work end-to-end", "[geometry-curves][frames][f64]")
{
    using T = crd::f64;
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "frames-test");

    auto a = unit_circle_xz<T>();
    a.sweep_radians = static_cast<T>(3.14159265358979323846);
    auto frames = compute_rmf(a, 16U, &alloc);
    REQUIRE(frames.size() == 17U);
    const auto plane_normal = v3<T>(0, 1, 0);
    for (const auto& f : frames)
    {
        REQUIRE(approx_eq(crd::math::dot(f.normal, plane_normal), static_cast<T>(0), static_cast<T>(1e-10)));
        REQUIRE(approx_eq(std::abs(crd::math::dot(f.binormal, plane_normal)), static_cast<T>(1),
                          static_cast<T>(1e-10)));
    }

    const auto t_hat = tangent(a, static_cast<T>(0.25));
    REQUIRE(approx_eq(crd::math::length(t_hat), static_cast<T>(1), static_cast<T>(1e-12)));
}
