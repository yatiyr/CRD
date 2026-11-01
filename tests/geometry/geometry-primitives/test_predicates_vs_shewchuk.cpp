// crd-geometry-primitives — the adaptive predicates against Shewchuk's reference implementation.
//
// predicates.hpp ports Shewchuk's "Adaptive Precision Floating-Point Arithmetic and Fast Robust Geometric
// Predicates" (Discrete & Computational Geometry 18(3), 1997). This test holds the port to its source: Shewchuk's own
// predicates.c (reference/shewchuk-predicates.c, compiled verbatim as the crd-geometry-shewchuk-oracle object library
// under the tree's floating-point contract). Each input is evaluated three times, by the engine, by Shewchuk's
// adaptive predicate and by his exact one (expansion arithmetic throughout); the three signs must be the same, so an
// exact zero in one is an exact zero in all. The inputs come from seeded SplitMix64 streams
// (crd/core/seed_generators.hpp) in the families on which a floating-point determinant goes wrong:
//   - uniform:  coordinates uniform in [-1, 1); the first stage decides nearly all of them.
//   - rounded:  the last point computed in floating point on the degenerate locus of the others (their line, plane,
//               circle or sphere), so it misses the locus by rounding only, then moved by up to two units in the last
//               place per coordinate. Every point is scaled by 2^-40, 1 or 2^40 and translated by up to 2^20 times
//               that scale, so the differences cancel in the leading bits. These reach the later stages and the
//               exact tail.
//   - lattice:  integer points exactly on the locus (a line, a plane, the circle x^2 + y^2 = 1105^2 and the sphere
//               x^2 + y^2 + z^2 = 45^2, each translated by up to 2^20), where every predicate must return exactly
//               zero, and the same points with one coordinate of the last moved by one, the nearest nonzero inputs.
// Non-finite input is the engine's own contract (zero), not Shewchuk's, and test_predicates.cpp covers it.

#include <crd/core/seed_generators.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdio>

namespace shewchuk
{
extern "C"
{
    // reference/shewchuk-predicates.c: a point is an array of two or three doubles. exactinit() computes the
    // machine epsilon, the splitter and the error bounds, and must run before any predicate.
    void exactinit();
    double orient2d(double* pa, double* pb, double* pc);
    double orient2dexact(double* pa, double* pb, double* pc);
    double orient3d(double* pa, double* pb, double* pc, double* pd);
    double orient3dexact(double* pa, double* pb, double* pc, double* pd);
    double incircle(double* pa, double* pb, double* pc, double* pd);
    double incircleexact(double* pa, double* pb, double* pc, double* pd);
    double insphere(double* pa, double* pb, double* pc, double* pd, double* pe);
    double insphereexact(double* pa, double* pb, double* pc, double* pd, double* pe);
}
} // namespace shewchuk

namespace
{
using crd::f64;
using crd::i64;
using crd::u64;
using crd::math::Vec2;
using crd::math::Vec3;
namespace pred = crd::geometry::primitives;

constexpr int kRounded = 6000;
constexpr int kUniform = 2000;
constexpr int kLattice = 2000;
constexpr f64 kScales[] = {0x1.0p-40, 1.0, 0x1.0p40};
constexpr f64 kOffsets[] = {0.0, 1.0, 1024.0 + 1.0 / 3.0, 1048576.0 + 1.0 / 7.0};

void init_oracle()
{
    static const bool kInitialized = []
    {
        shewchuk::exactinit();
        return true;
    }();
    (void)kInitialized;
}

class Stream
{
public:
    explicit Stream(u64 seed) noexcept : m_state(seed)
    {
    }

    u64 bits() noexcept
    {
        return crd::splitmix64_next(m_state);
    }

    // Uniform in [lo, hi), from 53 random bits.
    f64 uniform(f64 lo, f64 hi) noexcept
    {
        return lo + (hi - lo) * (static_cast<f64>(bits() >> 11U) * 0x1.0p-53);
    }

    // Uniform in [lo, hi], both small enough that the modulo bias is immaterial.
    i64 integer(i64 lo, i64 hi) noexcept
    {
        return lo + static_cast<i64>(bits() % static_cast<u64>(hi - lo + 1));
    }

private:
    u64 m_state;
};

// v moved by `ulps` units in its last place (toward larger magnitude when positive); zero stays zero, so no input
// becomes subnormal.
f64 nudge(f64 v, i64 ulps) noexcept
{
    if (v == 0.0)
    {
        return v;
    }
    return std::bit_cast<f64>(std::bit_cast<u64>(v) + static_cast<u64>(ulps));
}

int sign_of(f64 v) noexcept
{
    return (v > 0.0) - (v < 0.0);
}

// The signs one family produced, and the cases where the engine or Shewchuk's adaptive predicate left the exact sign.
struct Tally
{
    u64 cases = 0;
    u64 positive = 0;
    u64 negative = 0;
    u64 zero = 0;
    u64 engine_off = 0;
    u64 adaptive_off = 0;
};

// Judges one input: points `n` points of `dim` coordinates, the engine's and Shewchuk's two results. A disagreement is
// reported with the input in hexadecimal floating point, so it can be replayed exactly.
void judge(Tally& tally, const char* what, const f64 (*points)[3], int n, int dim, f64 engine, f64 adaptive,
           f64 exact)
{
    ++tally.cases;
    const int truth = sign_of(exact);
    tally.positive += truth > 0 ? 1U : 0U;
    tally.negative += truth < 0 ? 1U : 0U;
    tally.zero += truth == 0 ? 1U : 0U;
    const bool engine_ok = sign_of(engine) == truth;
    const bool adaptive_ok = sign_of(adaptive) == truth;
    tally.engine_off += engine_ok ? 0U : 1U;
    tally.adaptive_off += adaptive_ok ? 0U : 1U;
    if (engine_ok && adaptive_ok)
    {
        return;
    }
    char text[512];
    int used = std::snprintf(text, sizeof(text), "%s: engine %a, Shewchuk adaptive %a, exact %a at", what, engine,
                             adaptive, exact);
    for (int p = 0; p < n && used > 0 && used < static_cast<int>(sizeof(text)); ++p)
    {
        const std::size_t room = sizeof(text) - static_cast<std::size_t>(used);
        used += dim == 2 ? std::snprintf(text + used, room, " (%a, %a)", points[p][0], points[p][1])
                         : std::snprintf(text + used, room, " (%a, %a, %a)", points[p][0], points[p][1], points[p][2]);
    }
    FAIL_CHECK(text);
}

// The four predicates on one input, through both implementations.
void judge_orient2d(Tally& tally, const char* family, f64 (*p)[3])
{
    const f64 engine = pred::orient2d(Vec2<f64>(p[0][0], p[0][1]), Vec2<f64>(p[1][0], p[1][1]),
                                      Vec2<f64>(p[2][0], p[2][1]));
    judge(tally, family, p, 3, 2, engine, shewchuk::orient2d(p[0], p[1], p[2]),
          shewchuk::orient2dexact(p[0], p[1], p[2]));
}

void judge_orient3d(Tally& tally, const char* family, f64 (*p)[3])
{
    const f64 engine =
        pred::orient3d(Vec3<f64>(p[0][0], p[0][1], p[0][2]), Vec3<f64>(p[1][0], p[1][1], p[1][2]),
                       Vec3<f64>(p[2][0], p[2][1], p[2][2]), Vec3<f64>(p[3][0], p[3][1], p[3][2]));
    judge(tally, family, p, 4, 3, engine, shewchuk::orient3d(p[0], p[1], p[2], p[3]),
          shewchuk::orient3dexact(p[0], p[1], p[2], p[3]));
}

void judge_incircle(Tally& tally, const char* family, f64 (*p)[3])
{
    const f64 engine = pred::incircle(Vec2<f64>(p[0][0], p[0][1]), Vec2<f64>(p[1][0], p[1][1]),
                                      Vec2<f64>(p[2][0], p[2][1]), Vec2<f64>(p[3][0], p[3][1]));
    judge(tally, family, p, 4, 2, engine, shewchuk::incircle(p[0], p[1], p[2], p[3]),
          shewchuk::incircleexact(p[0], p[1], p[2], p[3]));
}

void judge_insphere(Tally& tally, const char* family, f64 (*p)[3])
{
    const f64 engine =
        pred::insphere(Vec3<f64>(p[0][0], p[0][1], p[0][2]), Vec3<f64>(p[1][0], p[1][1], p[1][2]),
                       Vec3<f64>(p[2][0], p[2][1], p[2][2]), Vec3<f64>(p[3][0], p[3][1], p[3][2]),
                       Vec3<f64>(p[4][0], p[4][1], p[4][2]));
    judge(tally, family, p, 5, 3, engine, shewchuk::insphere(p[0], p[1], p[2], p[3], p[4]),
          shewchuk::insphereexact(p[0], p[1], p[2], p[3], p[4]));
}

// Every point of `count` scaled by `scale` and translated by `offset` times it, per coordinate.
void place(f64 (*p)[3], int count, int dim, f64 scale, f64 offset)
{
    for (int i = 0; i < count; ++i)
    {
        for (int k = 0; k < dim; ++k)
        {
            p[i][k] = p[i][k] * scale + offset * scale;
        }
    }
}

// The last point moved by up to two units in the last place per coordinate.
void nudge_last(Stream& stream, f64 (*p)[3], int last, int dim)
{
    for (int k = 0; k < dim; ++k)
    {
        p[last][k] = nudge(p[last][k], stream.integer(-2, 2));
    }
}

void uniform_points(Stream& stream, f64 (*p)[3], int count, int dim)
{
    for (int i = 0; i < count; ++i)
    {
        for (int k = 0; k < dim; ++k)
        {
            p[i][k] = stream.uniform(-1.0, 1.0);
        }
    }
}

// A point of the unit circle or sphere from rational parameters (no trigonometry): (1 - t^2, 2t) / (1 + t^2), and the
// inverse stereographic projection (2u, 2v, u^2 + v^2 - 1) / (u^2 + v^2 + 1).
void on_circle(Stream& stream, f64* out, const f64* center, f64 radius)
{
    const f64 t = stream.uniform(-3.0, 3.0);
    const f64 den = 1.0 + t * t;
    out[0] = center[0] + radius * ((1.0 - t * t) / den);
    out[1] = center[1] + radius * ((2.0 * t) / den);
}

void on_sphere(Stream& stream, f64* out, const f64* center, f64 radius)
{
    const f64 u = stream.uniform(-2.0, 2.0);
    const f64 v = stream.uniform(-2.0, 2.0);
    const f64 den = u * u + v * v + 1.0;
    out[0] = center[0] + radius * ((2.0 * u) / den);
    out[1] = center[1] + radius * ((2.0 * v) / den);
    out[2] = center[2] + radius * ((u * u + v * v - 1.0) / den);
}

// The integer points of the circle x^2 + y^2 = r^2 or the sphere x^2 + y^2 + z^2 = r^2, by exhaustive search.
template <int Capacity>
struct LatticeSet
{
    f64 points[Capacity][3] = {};
    int count = 0;
};

bool exact_root(i64 n, i64& root) noexcept
{
    if (n < 0)
    {
        return false;
    }
    i64 r = 0;
    while ((r + 1) * (r + 1) <= n)
    {
        ++r;
    }
    root = r;
    return r * r == n;
}

template <int Capacity>
LatticeSet<Capacity> lattice(i64 radius, int dim)
{
    LatticeSet<Capacity> set;
    for (i64 x = -radius; x <= radius; ++x)
    {
        const i64 y_min = dim == 2 ? 0 : -radius;
        const i64 y_max = dim == 2 ? 0 : radius;
        for (i64 y = y_min; y <= y_max; ++y)
        {
            i64 last = 0;
            if (!exact_root(radius * radius - x * x - y * y, last))
            {
                continue;
            }
            // The last coordinate and, unless it is zero, its negation.
            const int signs = last == 0 ? 1 : 2;
            for (int k = 0; k < signs; ++k)
            {
                const i64 s = k == 0 ? last : -last;
                if (set.count < Capacity)
                {
                    set.points[set.count][0] = static_cast<f64>(x);
                    set.points[set.count][1] = static_cast<f64>(dim == 2 ? s : y);
                    set.points[set.count][2] = dim == 2 ? 0.0 : static_cast<f64>(s);
                }
                ++set.count;
            }
        }
    }
    return set;
}

// `n` distinct points of the set, translated by one integer vector of up to 2^20 per coordinate.
template <int Capacity>
void pick(Stream& stream, const LatticeSet<Capacity>& set, f64 (*p)[3], int n, int dim)
{
    int chosen[5] = {};
    for (int i = 0; i < n; ++i)
    {
        bool fresh = false;
        while (!fresh)
        {
            chosen[i] = static_cast<int>(stream.integer(0, set.count - 1));
            fresh = true;
            for (int j = 0; j < i; ++j)
            {
                fresh = fresh && chosen[j] != chosen[i];
            }
        }
    }
    f64 shift[3] = {};
    for (int k = 0; k < dim; ++k)
    {
        shift[k] = static_cast<f64>(stream.integer(-1048576, 1048576));
    }
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < dim; ++k)
        {
            p[i][k] = set.points[chosen[i]][k] + shift[k];
        }
    }
}

// The last point's first coordinate moved by one: the nearest lattice input off the locus.
void step_off(Stream& stream, f64 (*p)[3], int last)
{
    p[last][0] += stream.integer(0, 1) == 0 ? -1.0 : 1.0;
}

// Every case judged, none of them off the exact sign.
void require_agreement(const Tally& tally, int expected)
{
    CHECK(tally.cases == static_cast<u64>(expected));
    CHECK(tally.engine_off == 0U);
    CHECK(tally.adaptive_off == 0U);
}

// ... with both signs among them: the family straddles the locus.
void require_mixed(const Tally& tally, int expected)
{
    require_agreement(tally, expected);
    CHECK(tally.positive > 0U);
    CHECK(tally.negative > 0U);
}

// ... every one exactly zero: the family lies on the locus.
void require_zero(const Tally& tally, int expected)
{
    require_agreement(tally, expected);
    CHECK(tally.zero == static_cast<u64>(expected));
}

constexpr int kRoundedCases = kRounded * 3 * 4; // each case at each scale and offset

} // namespace

TEST_CASE("orient2d agrees with Shewchuk's adaptive and exact orient2d", "[predicates][shewchuk][orient2d]")
{
    init_oracle();
    Stream stream(0x5E3C'0D2D'0001ULL);
    f64 p[5][3] = {};
    Tally uniform;
    for (int i = 0; i < kUniform; ++i)
    {
        uniform_points(stream, p, 3, 2);
        judge_orient2d(uniform, "orient2d uniform", p);
    }
    Tally rounded;
    for (const f64 scale : kScales)
    {
        for (const f64 offset : kOffsets)
        {
            for (int i = 0; i < kRounded; ++i)
            {
                uniform_points(stream, p, 2, 2);
                place(p, 2, 2, scale, offset);
                const f64 t = stream.uniform(-1.0, 2.0);
                p[2][0] = p[0][0] + t * (p[1][0] - p[0][0]);
                p[2][1] = p[0][1] + t * (p[1][1] - p[0][1]);
                nudge_last(stream, p, 2, 2);
                judge_orient2d(rounded, "orient2d rounded", p);
            }
        }
    }
    Tally on_line;
    Tally off_line;
    for (int i = 0; i < kLattice; ++i)
    {
        const f64 a[2] = {static_cast<f64>(stream.integer(-16777216, 16777216)),
                          static_cast<f64>(stream.integer(-16777216, 16777216))};
        const f64 dir[2] = {static_cast<f64>(stream.integer(-4096, 4096)),
                            static_cast<f64>(stream.integer(-4096, 4096))};
        for (int j = 0; j < 3; ++j)
        {
            const f64 m = j == 0 ? 0.0 : static_cast<f64>(stream.integer(-8, 8));
            p[j][0] = a[0] + m * dir[0];
            p[j][1] = a[1] + m * dir[1];
        }
        judge_orient2d(on_line, "orient2d lattice on the line", p);
        step_off(stream, p, 2);
        judge_orient2d(off_line, "orient2d lattice off the line", p);
    }
    require_mixed(uniform, kUniform);
    require_mixed(rounded, kRoundedCases);
    require_zero(on_line, kLattice);
    require_agreement(off_line, kLattice);
}

TEST_CASE("orient3d agrees with Shewchuk's adaptive and exact orient3d", "[predicates][shewchuk][orient3d]")
{
    init_oracle();
    Stream stream(0x5E3C'0D3D'0002ULL);
    f64 p[5][3] = {};
    Tally uniform;
    for (int i = 0; i < kUniform; ++i)
    {
        uniform_points(stream, p, 4, 3);
        judge_orient3d(uniform, "orient3d uniform", p);
    }
    Tally rounded;
    for (const f64 scale : kScales)
    {
        for (const f64 offset : kOffsets)
        {
            for (int i = 0; i < kRounded; ++i)
            {
                uniform_points(stream, p, 3, 3);
                place(p, 3, 3, scale, offset);
                const f64 s = stream.uniform(-1.0, 2.0);
                const f64 t = stream.uniform(-1.0, 2.0);
                for (int k = 0; k < 3; ++k)
                {
                    p[3][k] = p[0][k] + s * (p[1][k] - p[0][k]) + t * (p[2][k] - p[0][k]);
                }
                nudge_last(stream, p, 3, 3);
                judge_orient3d(rounded, "orient3d rounded", p);
            }
        }
    }
    Tally on_plane;
    Tally off_plane;
    for (int i = 0; i < kLattice; ++i)
    {
        f64 a[3] = {};
        f64 u[3] = {};
        f64 v[3] = {};
        for (int k = 0; k < 3; ++k)
        {
            a[k] = static_cast<f64>(stream.integer(-1048576, 1048576));
            u[k] = static_cast<f64>(stream.integer(-1024, 1024));
            v[k] = static_cast<f64>(stream.integer(-1024, 1024));
        }
        // a, a + u, a + v and a + m u + n v.
        const f64 m[4] = {0.0, 1.0, 0.0, static_cast<f64>(stream.integer(-8, 8))};
        const f64 n[4] = {0.0, 0.0, 1.0, static_cast<f64>(stream.integer(-8, 8))};
        for (int j = 0; j < 4; ++j)
        {
            for (int k = 0; k < 3; ++k)
            {
                p[j][k] = a[k] + m[j] * u[k] + n[j] * v[k];
            }
        }
        judge_orient3d(on_plane, "orient3d lattice on the plane", p);
        step_off(stream, p, 3);
        judge_orient3d(off_plane, "orient3d lattice off the plane", p);
    }
    require_mixed(uniform, kUniform);
    require_mixed(rounded, kRoundedCases);
    require_zero(on_plane, kLattice);
    require_agreement(off_plane, kLattice);
}

TEST_CASE("incircle agrees with Shewchuk's adaptive and exact incircle", "[predicates][shewchuk][incircle]")
{
    init_oracle();
    Stream stream(0x5E3C'1CC1'0003ULL);
    f64 p[5][3] = {};
    Tally uniform;
    for (int i = 0; i < kUniform; ++i)
    {
        uniform_points(stream, p, 4, 2);
        judge_incircle(uniform, "incircle uniform", p);
    }
    Tally rounded;
    for (const f64 scale : kScales)
    {
        for (const f64 offset : kOffsets)
        {
            for (int i = 0; i < kRounded; ++i)
            {
                const f64 center[2] = {stream.uniform(-1.0, 1.0), stream.uniform(-1.0, 1.0)};
                const f64 radius = stream.uniform(0.25, 2.0);
                for (int j = 0; j < 4; ++j)
                {
                    on_circle(stream, p[j], center, radius);
                }
                place(p, 4, 2, scale, offset);
                nudge_last(stream, p, 3, 2);
                judge_incircle(rounded, "incircle rounded", p);
            }
        }
    }
    static const LatticeSet<256> kCircle = lattice<256>(1105, 2);
    REQUIRE(kCircle.count == 108); // 4 (2 + 1)^3: 1105^2 = 5^2 13^2 17^2, each prime 1 mod 4
    Tally on_circle_tally;
    Tally off_circle;
    for (int i = 0; i < kLattice; ++i)
    {
        pick(stream, kCircle, p, 4, 2);
        judge_incircle(on_circle_tally, "incircle lattice on the circle", p);
        step_off(stream, p, 3);
        judge_incircle(off_circle, "incircle lattice off the circle", p);
    }
    require_mixed(uniform, kUniform);
    require_mixed(rounded, kRoundedCases);
    require_zero(on_circle_tally, kLattice);
    require_mixed(off_circle, kLattice);
}

TEST_CASE("insphere agrees with Shewchuk's adaptive and exact insphere", "[predicates][shewchuk][insphere]")
{
    init_oracle();
    Stream stream(0x5E3C'15F4'0004ULL);
    f64 p[5][3] = {};
    Tally uniform;
    for (int i = 0; i < kUniform; ++i)
    {
        uniform_points(stream, p, 5, 3);
        judge_insphere(uniform, "insphere uniform", p);
    }
    Tally rounded;
    for (const f64 scale : kScales)
    {
        for (const f64 offset : kOffsets)
        {
            for (int i = 0; i < kRounded; ++i)
            {
                const f64 center[3] = {stream.uniform(-1.0, 1.0), stream.uniform(-1.0, 1.0),
                                       stream.uniform(-1.0, 1.0)};
                const f64 radius = stream.uniform(0.25, 2.0);
                for (int j = 0; j < 5; ++j)
                {
                    on_sphere(stream, p[j], center, radius);
                }
                place(p, 5, 3, scale, offset);
                nudge_last(stream, p, 4, 3);
                judge_insphere(rounded, "insphere rounded", p);
            }
        }
    }
    static const LatticeSet<2048> kSphere = lattice<2048>(45, 3);
    REQUIRE(kSphere.count > 100);
    REQUIRE(kSphere.count <= 2048);
    Tally on_sphere_tally;
    Tally off_sphere;
    for (int i = 0; i < kLattice; ++i)
    {
        pick(stream, kSphere, p, 5, 3);
        judge_insphere(on_sphere_tally, "insphere lattice on the sphere", p);
        step_off(stream, p, 4);
        judge_insphere(off_sphere, "insphere lattice off the sphere", p);
    }
    require_mixed(uniform, kUniform);
    require_mixed(rounded, kRoundedCases);
    require_zero(on_sphere_tally, kLattice);
    require_agreement(off_sphere, kLattice);
}
