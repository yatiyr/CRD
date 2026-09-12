#pragma once

// DerivedMassProperties + free `derive_mass_properties` — Phase 3.1
// v1a-material-d (ADR-0069 §3 + §8 + §11).
//
// Computes mass / centre-of-mass / inertia-tensor diagonal for a body's
// collider compound, given each collider's material density. The
// canonical implementation lives here as a free function so the cooker
// (v1k) and editor (Phase 7) can reuse it without instantiating a scene.
//
// Determinism contract (ADR-0063 §4 + ADR-0069 §9):
//   - Caller passes colliders in **ascending ColliderId order**. The
//     summation `mass = Σ (V_i · ρ_i)` and the COM weighted average run
//     in that exact order. FP `+` is commutative but not associative;
//     pinning the order matches "fixed-position write" protocol.
//   - The full inertia tensor is accumulated as a 3x3 symmetric matrix in
//     a fixed accumulation order; only the diagonal is returned (ADR
//     reserves off-diagonal handling for v1c diagonalisation, with
//     full-tensor side-channel storage in v1f for asymmetric compounds).
//
// Volume + inertia formulas:
//   Sphere   (r):              V = (4/3)π r³,           I = (2/5) m r² · I_3
//   Box      (hx, hy, hz):     V = 8 hx hy hz,          I_xx = (1/3) m (hy² + hz²) etc.
//   Capsule  (r, h, axis = Y): V = π r² (2h) + (4/3)π r³,
//                              I_yy = (1/2) m_cyl r² + (2/5) m_sph r²
//                              I_xx = I_zz = (1/12) m_cyl (3r² + 4h²)
//                                          + (83/320) m_sph r²
//                                          + m_sph (h + 3r/8)²
//                              where m_cyl/m_sph split by their volume share.
//   ConvexHull / Plane / TriangleMesh / Heightfield / Sdf:
//                              V = 0 in v1a; v1d-mesh / v1d-hf / v1d-sdf
//                              + cooker (v1k) supply precomputed mass
//                              properties for these. Plane is structurally
//                              static and stays at V = 0.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/collider.hpp>
#include <crd/eylem/material.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::eylem
{
struct DerivedMassProperties
{
    // Total mass, kg. Zero if the compound consists entirely of
    // zero-volume colliders (statics, planes, or v1a's deferred
    // mesh/hull/sdf cases).
    crd::f32         mass = 0.0F;

    // Centre of mass in body local frame. Equal to (0,0,0) when mass == 0.
    crd::math::Vec3f com_local{0.0F, 0.0F, 0.0F};

    // Diagonal of the body-frame inertia tensor about the body COM. v1a
    // returns the diagonal only — off-diagonal tensor terms are reserved
    // for v1f (asymmetric compounds) which adds a side-channel full
    // tensor; v1c-v1e diagonalise at body construction.
    crd::math::Vec3f inertia_diagonal{0.0F, 0.0F, 0.0F};
};

// Material accessor — caller-provided closure resolving MaterialId to a
// const Material&. Lets the function work both from a scene's
// MaterialPool AND from a cooker's offline material table.
//
// Signature: const Material& fn(MaterialId)
using MaterialAccessor = const Material& (*)(void* user_data, MaterialId id);

// Compute derived mass properties for a body whose colliders are passed
// in ascending ColliderId order (caller-enforced). Each collider's
// `material` field is resolved through `accessor` to read its density.
//
// `colliders.empty()` → all-zero result. The function never asserts on
// degenerate input (plane-only or all-static compounds); zero mass is a
// valid signal that the body is effectively static (matches RigidBody's
// `inv_mass = 0` convention).
[[nodiscard]] DerivedMassProperties derive_mass_properties(
    crd::containers::ConstSpan<Collider> colliders,
    MaterialAccessor                     accessor,
    void*                                user_data) noexcept;

} // namespace crd::eylem
