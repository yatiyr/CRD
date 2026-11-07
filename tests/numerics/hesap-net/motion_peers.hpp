// HESAP-NET's replicated-motion peer, behind plain calls: networked-physics-gdc-2015's snapshot interpolation, cut
// verbatim from the pinned Snapshot.h, with vectorial's nlerp and slerp, cut verbatim from the pinned quat4f.h
// (motion_peers.cpp).

#pragma once

#include <cstddef>

namespace motion_peers
{

// A body as the peer's snapshot holds it.
struct Body
{
    float position[3];
    float orientation[4];
    float velocity[3];
};

struct Pose
{
    float position[3];
    float orientation[4];
};

// InterpolateSnapshot_Linear, _Hermite and _Hermite_WithExtrapolation, one body at a time; each takes the orientation by
// the peer's slerp.
void linear(float t, const Body& a, const Body& b, Pose& out);
void hermite(float t, float step_size, const Body& a, const Body& b, Pose& out);
void hermite_extrapolated(float t, float step_size, float extrapolation, const Body& a, const Body& b, Pose& out);

// vectorial::nlerp and vectorial::slerp.
void nlerp(float t, const float* a, const float* b, float* out);
void slerp(float t, const float* a, const float* b, float* out);

// The board's arms: a whole snapshot by the same functions.
void linear_batch(float t, const Body* a, const Body* b, std::size_t count, Pose* out);
void hermite_batch(float t, float step_size, const Body* a, const Body* b, std::size_t count, Pose* out);
void nlerp_batch(float t, const float* a, const float* b, std::size_t count, float* out);
void slerp_batch(float t, const float* a, const float* b, std::size_t count, float* out);

} // namespace motion_peers
