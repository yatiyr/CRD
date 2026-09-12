#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7h Taubin smoothing (Taubin 1995).
//
// Volume-preserving Laplacian smoothing. The classical Laplacian smoother
// (`v ← v + λ · (centroid(neighbours) - v)` with `λ > 0`) reduces noise but
// monotonically SHRINKS the mesh on each iteration (a sphere collapses to
// a point; a cube melts to its centroid). Taubin's 1995 insight:
// alternate the Laplacian smooth with a NEGATIVE-weighted "un-shrink"
// step (`μ < 0`, `|μ| > λ`) — the combination acts as a frequency-domain
// LOW-PASS FILTER, removing high-frequency noise while preserving
// low-frequency shape. After N iteration pairs, the mesh has its noise
// stripped but its volume / silhouette / curvature drift ≈ 0.
//
// **Algorithm (per iteration pair):**
//
//   Pass 1 (shrink, λ > 0):
//     for each non-boundary vertex v:
//       L(v) = (1/n) · Σ neighbour_i  -  v       (umbrella operator)
//       v' = v + λ · L(v)
//     apply atomically (Jacobi update — order-independent, deterministic).
//
//   Pass 2 (un-shrink, μ < 0, typically `μ ≈ -1.04 · λ`):
//     same formula with μ instead of λ.
//
//   Repeat for `n_iterations` pairs (5-10 typical).
//
// **Frequency transfer function** (Taubin 1995 eq. 9):
//   f(k) = (1 - λ·k)(1 - μ·k)
//   where k ∈ [0, 2] is the eigenvalue of the discrete Laplacian (k = 0
//   for constant signals, k = 2 for highest frequencies).
//
//   For volume preservation: choose μ such that f(k_PB) = 1 for some
//   small "pass-band frequency" `k_PB > 0`. Then `(1/λ + 1/μ) = k_PB`,
//   giving `μ = λ / (k_PB · λ - 1)`. The default (λ=0.5, μ=-0.53) gives
//   `k_PB ≈ 0.113` — wide-pass-band suitable for general noise removal.
//
// **Boundary vertices**: default behaviour is to CLAMP (not move). For
// open meshes, smoothing boundary vertices requires the cubic-B-spline
// boundary mask from v7c — folded in via `boundary_smoothing` option
// when needed. v7h ships clamp-by-default.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11):** vertex iteration in
// slot order; Jacobi-style update (compute all new positions against
// OLD, apply atomically); arithmetic only — no transcendentals. Byte-
// identical output across compilers given byte-identical input.
//
// **Builder-reject / query-tolerate (ADR-0076 §15):** input must be
// 2-manifold (else `NonManifoldInput`); non-finite positions rejected
// by the underlying `HalfEdgeMesh::build_from`. `lambda <= 0` or
// `mu >= 0` → `InvalidParameters`.
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` body;
// typed wrappers in `taubin_smooth_typed.hpp` ship at slice close on
// first typed consumer.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

enum class TaubinSmoothStatus : crd::u8
{
    Ok                  = 0,
    EmptyMesh           = 1, // input has 0 faces
    NonManifoldInput    = 2, // input fails is_manifold()
    InvalidParameters   = 3, // lambda <= 0 or mu >= 0
};

template <crd::math::MathScalar T>
struct TaubinSmoothOptions
{
    // Number of (λ, μ) iteration pairs (each pair = one shrink + one
    // un-shrink pass). Standard range: 5-10. More iterations → more
    // noise removal; volume drift remains ≈ 0.
    crd::u32 n_iterations = 5;

    // Positive smoothing strength (the classical Laplacian step factor).
    // Default 0.5 — strong noise reduction without over-smoothing in 1
    // pass. Together with the default μ gives Taubin's standard
    // wide-pass-band filter (k_PB ≈ 0.113).
    T lambda = T{0.5};

    // Negative un-shrink strength. Must satisfy `|μ| > λ` for the filter
    // to be low-pass. Default -0.53 gives the standard wide-pass-band
    // setting. Tighter (= more aggressive shape-preservation) values:
    // λ=0.6307, μ=-0.6732 (Taubin 1995 §5, k_PB ≈ 0.1).
    T mu = static_cast<T>(-0.53);

    // If true, boundary vertices are clamped (not moved). When false,
    // boundary vertices are smoothed by the same Taubin filter on their
    // boundary neighbours only (NOT all 1-ring neighbours). For closed
    // meshes this option has no effect.
    bool keep_boundary_fixed = true;

    // Allocator for the OUTPUT mesh + scratch. If null, the input
    // mesh's allocator is used.
    crd::memory::IAllocator* output_allocator = nullptr;
};

struct TaubinSmoothReport
{
    TaubinSmoothStatus status                    = TaubinSmoothStatus::Ok;
    crd::u32           iterations_run            = 0;
    crd::u32           vertices_smoothed         = 0;
    crd::u32           boundary_vertices_clamped = 0;
    crd::u32           output_vertices           = 0;
    crd::u32           output_faces              = 0;
};

// Entry point. Builds a fresh smoothed copy of input; input is unmodified.
template <crd::math::MathScalar T>
HalfEdgeMesh<T> taubin_smooth(const HalfEdgeMesh<T>&             input,
                               const TaubinSmoothOptions<T>&     opts,
                               TaubinSmoothReport*               out_report = nullptr);

} // namespace crd::geometry::mesh_processing
