// crd-geometry-primitives v0d -- Tetrahedron type + barycentric / contains /
// volume / centroid + from_barycentric + the 3-tetrahedron prism decomposition.
// Triangle barycentric is already covered in test_primitives.cpp; this exercises
// the tetra forms + the reconstruction + the wedge split.

#include <crd/geometry/primitives/barycentric.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd;
using namespace crd::math;
using namespace crd::geometry::primitives;

namespace
{
template <typename T> constexpr T tol() noexcept
{
    return std::is_same_v<T, float> ? static_cast<T>(1e-4) : static_cast<T>(1e-9);
}
template <typename T> void close3(const Vec3<T>& a, const Vec3<T>& b, T eps = tol<T>())
{
    REQUIRE(approx_equal_abs(a.x, b.x, eps));
    REQUIRE(approx_equal_abs(a.y, b.y, eps));
    REQUIRE(approx_equal_abs(a.z, b.z, eps));
}
template <typename T> void close2(const Vec2<T>& a, const Vec2<T>& b, T eps = tol<T>())
{
    REQUIRE(approx_equal_abs(a.x, b.x, eps));
    REQUIRE(approx_equal_abs(a.y, b.y, eps));
}

struct Rng
{
    u64 s;
    explicit Rng(u64 seed) noexcept : s(seed) {}
    u64 next() noexcept
    {
        u64 z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    template <typename T> T uni(T lo, T hi) noexcept
    {
        const T u = static_cast<T>(next() >> 11) * (static_cast<T>(1) / static_cast<T>(1ULL << 53));
        return lo + u * (hi - lo);
    }
};
} // namespace

TEMPLATE_TEST_CASE("v0d -- Tetrahedron volume / centroid / orientation", "[geometry][barycentric]", float, double)
{
    using T = TestType;
    const Tetrahedron<T> unit(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0), Vec3<T>(0, 1, 0), Vec3<T>(0, 0, 1));
    REQUIRE(volume(unit) == Catch::Approx(static_cast<T>(1) / static_cast<T>(6)).margin(tol<T>()));
    REQUIRE(signed_volume(unit) == Catch::Approx(static_cast<T>(1) / static_cast<T>(6)).margin(tol<T>()));
    // Swapping two vertices flips the sign, keeps |volume|.
    const Tetrahedron<T> swapped(Vec3<T>(1, 0, 0), Vec3<T>(0, 0, 0), Vec3<T>(0, 1, 0), Vec3<T>(0, 0, 1));
    REQUIRE(signed_volume(swapped) == Catch::Approx(-static_cast<T>(1) / static_cast<T>(6)).margin(tol<T>()));
    REQUIRE(volume(swapped) == Catch::Approx(volume(unit)).margin(tol<T>()));
    close3(centroid(unit), Vec3<T>(static_cast<T>(0.25), static_cast<T>(0.25), static_cast<T>(0.25)));
}

TEMPLATE_TEST_CASE("v0d -- barycentric(Tetrahedron, p) and contains", "[geometry][barycentric]", float, double)
{
    using T = TestType;
    const Tetrahedron<T> tet(Vec3<T>(0, 0, 0), Vec3<T>(2, 0, 0), Vec3<T>(0, 2, 0), Vec3<T>(0, 0, 2));
    SECTION("vertices -> unit basis; centroid -> (1/4,1/4,1/4,1/4); weights sum to 1")
    {
        const Vec4<T> wa = barycentric(tet, tet.a);
        REQUIRE(wa.x == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
        REQUIRE((wa.y + wa.z + wa.w) == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        const Vec4<T> wc = barycentric(tet, tet.c);
        REQUIRE(wc.z == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
        const Vec3<T> g = centroid(tet);
        const Vec4<T> wg = barycentric(tet, g);
        REQUIRE(wg.x == Catch::Approx(static_cast<T>(0.25)).margin(tol<T>()));
        REQUIRE(wg.y == Catch::Approx(static_cast<T>(0.25)).margin(tol<T>()));
        REQUIRE(wg.z == Catch::Approx(static_cast<T>(0.25)).margin(tol<T>()));
        REQUIRE(wg.w == Catch::Approx(static_cast<T>(0.25)).margin(tol<T>()));
        REQUIRE((wg.x + wg.y + wg.z + wg.w) == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
    }
    SECTION("contains: inside / on a face / on an edge / on a vertex / outside")
    {
        REQUIRE(contains(tet, centroid(tet)));
        REQUIRE(contains(tet, Vec3<T>(static_cast<T>(0.5), static_cast<T>(0.5), 0))); // on face abc (z=0)
        REQUIRE(contains(tet, Vec3<T>(1, 0, 0)));                                     // on edge ab
        REQUIRE(contains(tet, tet.d));                                                // a vertex
        REQUIRE_FALSE(contains(tet, Vec3<T>(static_cast<T>(-0.1), 0, 0))); // just outside face bcd... actually face acd
        REQUIRE_FALSE(contains(tet, Vec3<T>(1, 1, 1)));                    // beyond face bcd
        REQUIRE_FALSE(contains(tet, Vec3<T>(5, 5, 5)));
    }
    SECTION("orientation-independence: a negatively-oriented tetra still works")
    {
        const Tetrahedron<T> neg(tet.b, tet.a, tet.c, tet.d); // swap a,b -> negative volume
        REQUIRE(signed_volume(neg) < static_cast<T>(0));
        REQUIRE(contains(neg, centroid(neg)));
        const Vec4<T> w = barycentric(neg, centroid(neg));
        REQUIRE((w.x + w.y + w.z + w.w) == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
        REQUIRE(w.x >= static_cast<T>(-1e-3));
    }
    SECTION("property: barycentric o from_barycentric round-trips a random interior point")
    {
        Rng rng(0xD0D4U);
        for (int it = 0; it < 64; ++it)
        {
            // Sample a point inside via random sub-simplex weights (Dirichlet-ish via sorted uniforms).
            T r1 = rng.uni<T>(0, 1);
            T r2 = rng.uni<T>(0, 1);
            T r3 = rng.uni<T>(0, 1);
            // fold into the standard tetra (Rocchini & Cignoni 2000)
            if (r1 + r2 > static_cast<T>(1))
            {
                r1 = static_cast<T>(1) - r1;
                r2 = static_cast<T>(1) - r2;
            }
            if (r2 + r3 > static_cast<T>(1))
            {
                const T t3 = r3;
                r3 = static_cast<T>(1) - r1 - r2;
                r2 = static_cast<T>(1) - t3;
            }
            else if (r1 + r2 + r3 > static_cast<T>(1))
            {
                const T t3 = r3;
                r3 = r1 + r2 + r3 - static_cast<T>(1);
                r1 = static_cast<T>(1) - r2 - t3;
            }
            const T r0 = static_cast<T>(1) - r1 - r2 - r3;
            const Vec4<T> w(r0, r1, r2, r3);
            const Vec3<T> p = from_barycentric(tet, w);
            REQUIRE(contains(tet, p, static_cast<T>(1e-4)));
            const Vec4<T> w2 = barycentric(tet, p);
            REQUIRE(w2.x == Catch::Approx(w.x).margin(static_cast<T>(2e-4)));
            REQUIRE(w2.y == Catch::Approx(w.y).margin(static_cast<T>(2e-4)));
            REQUIRE(w2.z == Catch::Approx(w.z).margin(static_cast<T>(2e-4)));
            REQUIRE(w2.w == Catch::Approx(w.w).margin(static_cast<T>(2e-4)));
        }
    }
}

TEMPLATE_TEST_CASE("v0d -- from_barycentric (Triangle3 / Triangle2 / Tetrahedron)", "[geometry][barycentric]", float,
                   double)
{
    using T = TestType;
    const Triangle3<T> t3(Vec3<T>(0, 0, 0), Vec3<T>(4, 0, 0), Vec3<T>(0, 4, 0));
    close3(from_barycentric(t3, Vec3<T>(1, 0, 0)), Vec3<T>(0, 0, 0));
    close3(from_barycentric(t3, Vec3<T>(static_cast<T>(1) / 3, static_cast<T>(1) / 3, static_cast<T>(1) / 3)),
           centroid(t3));
    // round-trip a random interior point of the triangle
    Rng rng(0xBA12U);
    for (int it = 0; it < 32; ++it)
    {
        T u = rng.uni<T>(0, 1);
        T v = rng.uni<T>(0, 1);
        if (u + v > static_cast<T>(1))
        {
            u = static_cast<T>(1) - u;
            v = static_cast<T>(1) - v;
        }
        const Vec3<T> w(static_cast<T>(1) - u - v, u, v);
        const Vec3<T> p = from_barycentric(t3, w);
        const Vec3<T> w2 = barycentric(t3, p);
        REQUIRE(w2.x == Catch::Approx(w.x).margin(static_cast<T>(2e-4)));
        REQUIRE(w2.y == Catch::Approx(w.y).margin(static_cast<T>(2e-4)));
        REQUIRE(w2.z == Catch::Approx(w.z).margin(static_cast<T>(2e-4)));
    }
    const Triangle2<T> t2(Vec2<T>(0, 0), Vec2<T>(4, 0), Vec2<T>(0, 4));
    close2(from_barycentric(t2, Vec3<T>(0, 1, 0)), Vec2<T>(4, 0));

    const Tetrahedron<T> tet(Vec3<T>(0, 0, 0), Vec3<T>(3, 0, 0), Vec3<T>(0, 3, 0), Vec3<T>(0, 0, 3));
    close3(from_barycentric(tet, Vec4<T>(0, 0, 0, 1)), Vec3<T>(0, 0, 3));
    close3(from_barycentric(
               tet, Vec4<T>(static_cast<T>(0.25), static_cast<T>(0.25), static_cast<T>(0.25), static_cast<T>(0.25))),
           centroid(tet));
}

TEMPLATE_TEST_CASE("v0d -- 3-tetrahedron prism decomposition", "[geometry][barycentric]", float, double)
{
    using T = TestType;
    // A right prism: bottom triangle at z=0, top at z=h.
    const Triangle3<T> bottom(Vec3<T>(0, 0, 0), Vec3<T>(2, 0, 0), Vec3<T>(0, 3, 0));
    const T h = static_cast<T>(1.5);
    const Triangle3<T> top(Vec3<T>(0, 0, h), Vec3<T>(2, 0, h), Vec3<T>(0, 3, h));
    const auto tets = decompose_prism_to_tets(bottom, top);

    REQUIRE(tets.size() == 3U);
    SECTION("non-degenerate; volumes sum to base-area * height")
    {
        T sum = static_cast<T>(0);
        for (const Tetrahedron<T>& tet : tets)
        {
            REQUIRE(volume(tet) > tol<T>());
            sum += volume(tet);
        }
        const T base_area = static_cast<T>(0.5) * static_cast<T>(2) * static_cast<T>(3); // = 3
        REQUIRE(sum == Catch::Approx(base_area * h).margin(static_cast<T>(2e-3)));
    }
    SECTION("every prism vertex is a vertex of >= 1 tet")
    {
        const Vec3<T> verts[6] = {bottom.a, bottom.b, bottom.c, top.a, top.b, top.c};
        for (const Vec3<T>& vtx : verts)
        {
            bool found = false;
            for (const Tetrahedron<T>& tet : tets)
            {
                if (vtx == tet.a || vtx == tet.b || vtx == tet.c || vtx == tet.d)
                {
                    found = true;
                }
            }
            REQUIRE(found);
        }
    }
    SECTION("the union covers the prism (no gaps), and the tets don't overflow it")
    {
        const Triangle2<T> base2(Vec2<T>(0, 0), Vec2<T>(2, 0), Vec2<T>(0, 3));
        Rng rng(0x3737U);
        for (int it = 0; it < 128; ++it)
        {
            // Random point in the prism's bounding box.
            const Vec3<T> p(rng.uni<T>(static_cast<T>(-1), static_cast<T>(3)),
                            rng.uni<T>(static_cast<T>(-1), static_cast<T>(4)),
                            rng.uni<T>(static_cast<T>(-1), static_cast<T>(3)));
            const bool xy_in_strict = contains(base2, Vec2<T>(p.x, p.y), static_cast<T>(-1e-3));
            const bool xy_in_lenient = contains(base2, Vec2<T>(p.x, p.y), static_cast<T>(1e-3));
            const bool z_in_strict = p.z >= static_cast<T>(1e-3) && p.z <= h - static_cast<T>(1e-3);
            const bool z_in_lenient = p.z >= static_cast<T>(-1e-3) && p.z <= h + static_cast<T>(1e-3);
            int hits = 0;
            for (const Tetrahedron<T>& tet : tets)
            {
                if (contains(tet, p, static_cast<T>(1e-4)))
                {
                    ++hits;
                }
            }
            if (xy_in_strict && z_in_strict)
            {
                REQUIRE(hits >= 1); // clearly inside the prism => covered by >=1 tet (no gaps)
            }
            if (!(xy_in_lenient && z_in_lenient))
            {
                REQUIRE(hits == 0); // clearly outside => no tet claims it (no overflow)
            }
        }
    }
}
