// mas-bandwidth/serialize as its release ships it, and networked-physics' compressed_quaternion structs cut verbatim from
// the pinned Snapshot.h at configure time (the root CMakeLists), compiled on the few stand-ins below: `quat4f` holds the
// four floats, and `normalize` is the identity because that source's is an SSE reciprocal square root estimate, which
// differs by CPU and no wire carries. Included by peer code only (crd-networked-physics-peer).

#pragma once

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the release header reads this macro.
#define SERIALIZE_ENABLE_TESTS 0
#include <cmath>
#include <cstdint>
#include <cstring>
#include <serialize.h>

namespace vectorial
{

// NOLINTNEXTLINE(readability-identifier-naming): the name the cut source spells.
struct quat4f
{
    float v[4];

    quat4f() = default;

    quat4f(float x, float y, float z, float w) : v{x, y, z, w} {}

    void store(float* out) const { std::memcpy(out, v, sizeof(v)); }

    static quat4f identity() { return quat4f(0.0F, 0.0F, 0.0F, 1.0F); }
};

inline quat4f normalize(const quat4f& q)
{
    return q;
}

} // namespace vectorial

namespace math
{

inline float floor(float value)
{
    return std::floor(value);
}

} // namespace math

// Peer code runs only on well-formed bit widths.
#define CORE_ASSERT(condition)
// The structs' Serialize members are written for the demo's own stream macros, which return nothing: never instantiated.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage,bugprone-macro-parentheses): a declaration the cut source spells.
#define PROTOCOL_SERIALIZE_OBJECT(stream) template <typename Stream> bool Serialize(Stream& stream)

#include "networked_physics_quaternion_slice.inc"
