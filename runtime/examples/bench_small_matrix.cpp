// OWN.1's before/after board for geometry's closed-form small-matrix routines moved onto crd-math: each case times the
// moved function (`owner`: crd-math's determinant and inverse_transpose, and the geometry functions that now call
// crd-math's try_inverse and solve) and the copy it replaced (`reference`, written out here as it was) over the same
// inputs in one process, alternating which runs first, so scripts/ab-board.py reads both arms from one run. A line is
// `<case> <arm> <ns per item>`.

#include <crd/core/seed_generators.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/math/mat.hpp>

#include <chrono>
#include <cstdio>
#include <optional>

using crd::f32;
using crd::f64;
using crd::u64;
using crd::usize;
using crd::math::Mat3;
using crd::math::Vec2;
using crd::math::Vec3;

namespace
{
volatile f64 g_sink = 0.0;

constexpr usize kItems = 4096;

template <typename T> T reference_det3(const Mat3<T>& m)
{
    return m.c0.x * (m.c1.y * m.c2.z - m.c2.y * m.c1.z) - m.c0.y * (m.c1.x * m.c2.z - m.c2.x * m.c1.z) +
           m.c0.z * (m.c1.x * m.c2.y - m.c2.x * m.c1.y);
}

template <typename T> Mat3<T> reference_inverse_transpose_3(const Mat3<T>& m)
{
    const T cof00 = (m.c1.y * m.c2.z - m.c2.y * m.c1.z);
    const T cof01 = -(m.c1.x * m.c2.z - m.c2.x * m.c1.z);
    const T cof02 = (m.c1.x * m.c2.y - m.c2.x * m.c1.y);
    const T cof10 = -(m.c0.y * m.c2.z - m.c2.y * m.c0.z);
    const T cof11 = (m.c0.x * m.c2.z - m.c2.x * m.c0.z);
    const T cof12 = -(m.c0.x * m.c2.y - m.c2.x * m.c0.y);
    const T cof20 = (m.c0.y * m.c1.z - m.c1.y * m.c0.z);
    const T cof21 = -(m.c0.x * m.c1.z - m.c1.x * m.c0.z);
    const T cof22 = (m.c0.x * m.c1.y - m.c1.x * m.c0.y);
    const T det = m.c0.x * cof00 + m.c0.y * cof01 + m.c0.z * cof02;
    const T inv_det = static_cast<T>(1) / det;
    return Mat3<T>(Vec3<T>(cof00 * inv_det, cof01 * inv_det, cof02 * inv_det),
                   Vec3<T>(cof10 * inv_det, cof11 * inv_det, cof12 * inv_det),
                   Vec3<T>(cof20 * inv_det, cof21 * inv_det, cof22 * inv_det));
}

std::optional<Vec3<f64>> reference_optimal_position(const crd::geometry::mesh_processing::Quadric<f64>& q,
                                                    f64 det_epsilon)
{
    const f64 a = q.data[0];
    const f64 b = q.data[1];
    const f64 c = q.data[2];
    const f64 d = q.data[4];
    const f64 e = q.data[5];
    const f64 f = q.data[7];
    const f64 det = a * (d * f - e * e) - b * (b * f - c * e) + c * (b * e - c * d);
    if (crd::math::abs(det) < det_epsilon)
    {
        return std::nullopt;
    }
    const f64 rx = -q.data[3];
    const f64 ry = -q.data[6];
    const f64 rz = -q.data[8];
    const f64 inv_det = 1.0 / det;
    const f64 i00 = (d * f - e * e) * inv_det;
    const f64 i01 = -(b * f - c * e) * inv_det;
    const f64 i02 = (b * e - c * d) * inv_det;
    const f64 i11 = (a * f - c * c) * inv_det;
    const f64 i12 = -(a * e - b * c) * inv_det;
    const f64 i22 = (a * d - b * b) * inv_det;
    return Vec3<f64>{i00 * rx + i01 * ry + i02 * rz, i01 * rx + i11 * ry + i12 * rz, i02 * rx + i12 * ry + i22 * rz};
}

Vec2<f64> reference_circumcenter_2d(const Vec2<f64>& a, const Vec2<f64>& b, const Vec2<f64>& c)
{
    const f64 d = 2.0 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    if (d == 0.0)
    {
        return Vec2<f64>{(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0};
    }
    const f64 a_sq = a.x * a.x + a.y * a.y;
    const f64 b_sq = b.x * b.x + b.y * b.y;
    const f64 c_sq = c.x * c.x + c.y * c.y;
    return Vec2<f64>{(a_sq * (b.y - c.y) + b_sq * (c.y - a.y) + c_sq * (a.y - b.y)) / d,
                     (a_sq * (c.x - b.x) + b_sq * (a.x - c.x) + c_sq * (b.x - a.x)) / d};
}

Vec3<f64> reference_circumcenter_3d(const Vec3<f64>& a, const Vec3<f64>& b, const Vec3<f64>& c, const Vec3<f64>& d)
{
    const f64 bx = b.x - a.x;
    const f64 by = b.y - a.y;
    const f64 bz = b.z - a.z;
    const f64 cx = c.x - a.x;
    const f64 cy = c.y - a.y;
    const f64 cz = c.z - a.z;
    const f64 dx = d.x - a.x;
    const f64 dy = d.y - a.y;
    const f64 dz = d.z - a.z;
    const f64 det6 = bx * (cy * dz - cz * dy) - by * (cx * dz - cz * dx) + bz * (cx * dy - cy * dx);
    if (det6 == 0.0)
    {
        return Vec3<f64>{(4.0 * a.x + bx + cx + dx) / 4.0, (4.0 * a.y + by + cy + dy) / 4.0,
                         (4.0 * a.z + bz + cz + dz) / 4.0};
    }
    const f64 rhs_b = (bx * bx + by * by + bz * bz) * 0.5;
    const f64 rhs_c = (cx * cx + cy * cy + cz * cz) * 0.5;
    const f64 rhs_d = (dx * dx + dy * dy + dz * dz) * 0.5;
    const f64 inv_det = 1.0 / det6;
    const f64 ox = (rhs_b * (cy * dz - cz * dy) - by * (rhs_c * dz - cz * rhs_d) + bz * (rhs_c * dy - cy * rhs_d)) *
                   inv_det;
    const f64 oy = (bx * (rhs_c * dz - cz * rhs_d) - rhs_b * (cx * dz - cz * dx) + bz * (cx * rhs_d - rhs_c * dx)) *
                   inv_det;
    const f64 oz = (bx * (cy * rhs_d - rhs_c * dy) - by * (cx * rhs_d - rhs_c * dx) + rhs_b * (cx * dy - cy * dx)) *
                   inv_det;
    return Vec3<f64>{a.x + ox, a.y + oy, a.z + oz};
}

// Nanoseconds per item of `body()` over kItems inputs, the median of five timed runs after a warm one, each long
// enough (~20 ms) that the clock's resolution does not show.
template <typename Body> double ns_per_item(Body body)
{
    body();
    usize reps = 1U;
    for (;;)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body();
        }
        const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= 2.0e7)
        {
            break;
        }
        reps *= 2U;
    }
    double runs[5] = {};
    for (double& run : runs)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body();
        }
        run = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
              (static_cast<double>(reps) * static_cast<double>(kItems));
    }
    for (usize i = 1; i < 5U; ++i) // insertion sort of five
    {
        for (usize j = i; j > 0U && runs[j - 1U] > runs[j]; --j)
        {
            const double t = runs[j];
            runs[j] = runs[j - 1U];
            runs[j - 1U] = t;
        }
    }
    return runs[2];
}

// The arms run in alternating order from case to case, so in-process drift lands on both.
bool g_owner_first = true;

template <typename Owner, typename Reference> void row(const char* name, Owner owner, Reference reference)
{
    double owner_ns = 0.0;
    double reference_ns = 0.0;
    if (g_owner_first)
    {
        owner_ns = ns_per_item(owner);
        reference_ns = ns_per_item(reference);
    }
    else
    {
        reference_ns = ns_per_item(reference);
        owner_ns = ns_per_item(owner);
    }
    g_owner_first = !g_owner_first;
    std::printf("%s owner %.4f\n%s reference %.4f\n", name, owner_ns, name, reference_ns);
}

f64 unit_draw(u64& state)
{
    return static_cast<f64>(crd::splitmix64_next(state) >> 11U) * 0x1.0p-52 - 1.0;
}

template <typename T> Vec3<T> vec3_draw(u64& state)
{
    Vec3<T> v;
    v.x = static_cast<T>(unit_draw(state));
    v.y = static_cast<T>(unit_draw(state));
    v.z = static_cast<T>(unit_draw(state));
    return v;
}

// The inputs, drawn once; the arrays are static so the stack stays small.
Mat3<f32> g_mats[kItems];
crd::geometry::mesh_processing::Quadric<f64> g_quadrics[kItems];
Vec3<f64> g_points[4][kItems];

template <typename T> f64 sum(const Mat3<T>& m)
{
    return static_cast<f64>(m.c0.x + m.c1.y + m.c2.z + m.c0.y + m.c1.z + m.c2.x);
}
} // namespace

int main()
{
    u64 state = 0x5A11'3A7EULL;
    for (Mat3<f32>& m : g_mats)
    {
        m.c0 = vec3_draw<f32>(state);
        m.c1 = vec3_draw<f32>(state);
        m.c2 = vec3_draw<f32>(state);
    }
    for (auto& q : g_quadrics) // three planes each: the decimator's regular case
    {
        for (usize k = 0; k < 3U; ++k)
        {
            const Vec3<f64> n = vec3_draw<f64>(state);
            q += crd::geometry::mesh_processing::Quadric<f64>::from_plane(n.x, n.y, n.z, unit_draw(state));
        }
    }
    for (auto& points : g_points)
    {
        for (Vec3<f64>& p : points)
        {
            p = vec3_draw<f64>(state);
        }
    }

    std::printf("# OWN.1 small-matrix routines: crd-math (owner) vs the moved copies (reference), ns per item\n");
    row(
        "det3-f32",
        []
        {
            f64 acc = 0.0;
            for (const Mat3<f32>& m : g_mats)
            {
                acc += static_cast<f64>(crd::math::determinant(m));
            }
            g_sink = acc;
        },
        []
        {
            f64 acc = 0.0;
            for (const Mat3<f32>& m : g_mats)
            {
                acc += static_cast<f64>(reference_det3(m));
            }
            g_sink = acc;
        });
    row(
        "inverse-transpose-f32",
        []
        {
            f64 acc = 0.0;
            for (const Mat3<f32>& m : g_mats)
            {
                acc += sum(crd::math::inverse_transpose(m));
            }
            g_sink = acc;
        },
        []
        {
            f64 acc = 0.0;
            for (const Mat3<f32>& m : g_mats)
            {
                acc += sum(reference_inverse_transpose_3(m));
            }
            g_sink = acc;
        });
    row(
        "quadric-optimal-f64",
        []
        {
            f64 acc = 0.0;
            for (const auto& q : g_quadrics)
            {
                const auto v = crd::geometry::mesh_processing::optimal_position(q, 1e-10);
                acc += v.has_value() ? v->x + v->y + v->z : 1.0;
            }
            g_sink = acc;
        },
        []
        {
            f64 acc = 0.0;
            for (const auto& q : g_quadrics)
            {
                const auto v = reference_optimal_position(q, 1e-10);
                acc += v.has_value() ? v->x + v->y + v->z : 1.0;
            }
            g_sink = acc;
        });
    row(
        "circumcenter-2d",
        []
        {
            f64 acc = 0.0;
            for (usize i = 0; i < kItems; ++i)
            {
                const Vec2<f64> o = crd::geometry::primitives::circumcenter_2d(
                    Vec2<f64>{g_points[0][i].x, g_points[0][i].y}, Vec2<f64>{g_points[1][i].x, g_points[1][i].y},
                    Vec2<f64>{g_points[2][i].x, g_points[2][i].y});
                acc += o.x + o.y;
            }
            g_sink = acc;
        },
        []
        {
            f64 acc = 0.0;
            for (usize i = 0; i < kItems; ++i)
            {
                const Vec2<f64> o = reference_circumcenter_2d(Vec2<f64>{g_points[0][i].x, g_points[0][i].y},
                                                              Vec2<f64>{g_points[1][i].x, g_points[1][i].y},
                                                              Vec2<f64>{g_points[2][i].x, g_points[2][i].y});
                acc += o.x + o.y;
            }
            g_sink = acc;
        });
    row(
        "circumcenter-3d",
        []
        {
            f64 acc = 0.0;
            for (usize i = 0; i < kItems; ++i)
            {
                const Vec3<f64> o = crd::geometry::primitives::circumcenter_3d(g_points[0][i], g_points[1][i],
                                                                               g_points[2][i], g_points[3][i]);
                acc += o.x + o.y + o.z;
            }
            g_sink = acc;
        },
        []
        {
            f64 acc = 0.0;
            for (usize i = 0; i < kItems; ++i)
            {
                const Vec3<f64> o =
                    reference_circumcenter_3d(g_points[0][i], g_points[1][i], g_points[2][i], g_points[3][i]);
                acc += o.x + o.y + o.z;
            }
            g_sink = acc;
        });
    return 0;
}
