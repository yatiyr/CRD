// The peer of motion_peers.hpp. InterpolateSnapshot_* and hermite_spline are cut verbatim from the pinned
// networked-physics Snapshot.h, and nlerp and slerp from its external/vectorial/quat4f.h, at configure time
// (tests/numerics/hesap-net/CMakeLists.txt). They compile on the few stand-ins below, whose arithmetic is vectorial's
// scalar backend (simd4f_scalar.h, simd4f_common.h): each operator one IEEE operation per component, a dot product summed
// x, y, z, w from the left, and normalize as `a * (1.0f / sqrtf(dot))`. Its SSE backend's normalize is a reciprocal
// square root estimate instead, which differs by CPU; the scalar backend is the one defined result.
//
// Built with the engine's floating-point contract (no contraction), as the source requires of a comparison.

#include "motion_peers.hpp"

#include <cmath>

namespace vectorial
{

// NOLINTBEGIN(readability-identifier-naming): the names the cut source spells.
struct vec3f
{
    float x;
    float y;
    float z;
};

inline vec3f operator+(const vec3f& a, const vec3f& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline vec3f operator-(const vec3f& a, const vec3f& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline vec3f operator*(const vec3f& a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

inline vec3f operator*(float s, const vec3f& a)
{
    return {s * a.x, s * a.y, s * a.z};
}

struct quat4f
{
    float x;
    float y;
    float z;
    float w;
};

inline quat4f operator+(const quat4f& a, const quat4f& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

inline quat4f operator-(const quat4f& a, const quat4f& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

inline quat4f operator-(const quat4f& a)
{
    return {-a.x, -a.y, -a.z, -a.w};
}

inline quat4f operator*(const quat4f& a, float s)
{
    return {a.x * s, a.y * s, a.z * s, a.w * s};
}

inline float dot(const quat4f& a, const quat4f& b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

inline quat4f normalize(const quat4f& a)
{
    return a * (1.0F / std::sqrt(dot(a, a)));
}

using std::acos;
using std::sin;

#include "vectorial_quat4f_slice.inc"

} // namespace vectorial

namespace
{

// The few names the cut interpolators read.
constexpr int NumCubes = 1;
constexpr int MaxPlayers = 2;

namespace hypercube
{
constexpr float PlayerCubeSize = 1.0F;
constexpr float NonPlayerCubeSize = 1.0F;
} // namespace hypercube

namespace view
{
struct ObjectUpdate
{
    int id;
    vectorial::vec3f position;
    vectorial::quat4f orientation;
    float scale;
    int authority;
    bool visible;
};
} // namespace view

struct CubeState
{
    vectorial::vec3f position;
    vectorial::quat4f orientation;
    vectorial::vec3f linear_velocity;
    bool interacting;
};

// The source's own spelling of a qualifier no pointee takes.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage,bugprone-reserved-identifier)
#define __restrict
#include "networked_physics_interpolation_slice.inc"
#undef __restrict

// NOLINTEND(readability-identifier-naming)

[[nodiscard]] vectorial::vec3f vec3(const float* v)
{
    return {v[0], v[1], v[2]};
}

[[nodiscard]] vectorial::quat4f quat(const float* q)
{
    return {q[0], q[1], q[2], q[3]};
}

[[nodiscard]] CubeState cube(const motion_peers::Body& body)
{
    return {vec3(body.position), quat(body.orientation), vec3(body.velocity), false};
}

void store(const vectorial::quat4f& q, float* out)
{
    out[0] = q.x;
    out[1] = q.y;
    out[2] = q.z;
    out[3] = q.w;
}

void store(const view::ObjectUpdate& update, motion_peers::Pose& out)
{
    out.position[0] = update.position.x;
    out.position[1] = update.position.y;
    out.position[2] = update.position.z;
    store(update.orientation, out.orientation);
}

} // namespace

namespace motion_peers
{

void linear(float t, const Body& a, const Body& b, Pose& out)
{
    const CubeState from = cube(a);
    const CubeState to = cube(b);
    view::ObjectUpdate update{};
    InterpolateSnapshot_Linear(t, &from, &to, &update);
    store(update, out);
}

void hermite(float t, float step_size, const Body& a, const Body& b, Pose& out)
{
    const CubeState from = cube(a);
    const CubeState to = cube(b);
    view::ObjectUpdate update{};
    InterpolateSnapshot_Hermite(t, step_size, &from, &to, &update);
    store(update, out);
}

void hermite_extrapolated(float t, float step_size, float extrapolation, const Body& a, const Body& b, Pose& out)
{
    const CubeState from = cube(a);
    const CubeState to = cube(b);
    view::ObjectUpdate update{};
    InterpolateSnapshot_Hermite_WithExtrapolation(t, step_size, extrapolation, &from, &to, &update);
    store(update, out);
}

void nlerp(float t, const float* a, const float* b, float* out)
{
    store(vectorial::nlerp(t, quat(a), quat(b)), out);
}

void slerp(float t, const float* a, const float* b, float* out)
{
    store(vectorial::slerp(t, quat(a), quat(b)), out);
}

void linear_batch(float t, const Body* a, const Body* b, std::size_t count, Pose* out)
{
    for (std::size_t i = 0U; i < count; ++i)
    {
        linear(t, a[i], b[i], out[i]);
    }
}

void hermite_batch(float t, float step_size, const Body* a, const Body* b, std::size_t count, Pose* out)
{
    for (std::size_t i = 0U; i < count; ++i)
    {
        hermite(t, step_size, a[i], b[i], out[i]);
    }
}

void nlerp_batch(float t, const float* a, const float* b, std::size_t count, float* out)
{
    for (std::size_t i = 0U; i < count; ++i)
    {
        nlerp(t, a + (4U * i), b + (4U * i), out + (4U * i));
    }
}

void slerp_batch(float t, const float* a, const float* b, std::size_t count, float* out)
{
    for (std::size_t i = 0U; i < count; ++i)
    {
        slerp(t, a + (4U * i), b + (4U * i), out + (4U * i));
    }
}

} // namespace motion_peers
