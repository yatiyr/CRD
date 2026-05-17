// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7h Taubin smoothing implementation.
//
// See header for the algorithm contract. This TU contains:
//   - Per-vertex boundary detection (cache built once at entry).
//   - One-ring centroid + Laplacian step (with `λ` or `μ` factor).
//   - Jacobi-style atomic Pass-1 + Pass-2 application via
//     `HalfEdgeMesh::set_vertex_position` (added in v7d).
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D67. **Clone-and-mutate pattern.** Same as v7d/v7e/v7g: extract
//        input via `to_indexed`, rebuild output on the requested
//        allocator, then mutate the output through `set_vertex_position`.
//        Avoids HE-mesh move-semantics questions.
//
//   D68. **Boundary flag cached ONCE at entry.** For each vertex,
//        determine whether it's on the boundary via the same predicate
//        used in v7c/v7d (some outgoing or twin-of-outgoing HE has
//        `face == k_null_face`). The flag is constant across the
//        smoothing iterations (Taubin doesn't change topology), so
//        cache it instead of re-walking the 1-ring each pass.
//
//   D69. **Jacobi update per pass.** Each pass (Pass-1 with λ, Pass-2
//        with μ) computes new positions for ALL alive vertices against
//        OLD positions in a scratch array, then applies atomically.
//        Order-independent → deterministic across compilers.
//
//   D70. **Boundary handling**: if `keep_boundary_fixed == true`,
//        boundary vertices skip both passes (new position = old
//        position). If false, boundary vertices are smoothed using
//        ONLY their boundary 1-ring neighbours (= the two adjacent
//        vertices in the boundary loop) — matches the v7c cubic-
//        B-spline mask intent but with Taubin's λ/μ alternation
//        (which still acts as a low-pass on the boundary curve).
//
//   D71. **Umbrella operator** `L(v) = mean(neighbours) - v` (uniform-
//        weight, NOT cotangent-weighted). The uniform weight is
//        Taubin's original form; cotangent weighting (Pinkall-Polthier
//        / Desbrun) is a quality refinement that ships as a v7h-cotan
//        followon if a consumer requires it.
//
//   D72. **InvalidParameters** check: reject `lambda <= 0` (must be
//        positive to define the shrink step) and `mu >= 0` (must be
//        negative to define the un-shrink). Does NOT reject `|μ| <= λ`
//        (which would technically defeat the low-pass property) — the
//        caller may experimentally want high-pass filters, etc.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/taubin_smooth.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{
namespace
{

template <crd::math::MathScalar T>
bool is_boundary_vertex(const HalfEdgeMesh<T>& m, crd::u32 v) noexcept
{
    bool b = false;
    m.for_each_outgoing_he(v, [&](crd::u32 ho) {
        if (b) { return; }
        if (m.he_is_boundary(ho)) { b = true; return; }
        const crd::u32 t = m.he(ho).twin;
        if (t != k_null_he && m.he_is_boundary(t)) { b = true; }
    });
    return b;
}

// One Taubin pass with the given factor (λ for shrink, μ for un-shrink).
// Computes new positions against OLD into `scratch`; applies atomically.
template <crd::math::MathScalar T>
crd::u32 apply_taubin_pass(HalfEdgeMesh<T>&                            m,
                            const crd::containers::Array<crd::u8>&      is_boundary,
                            T                                            factor,
                            bool                                         keep_boundary_fixed,
                            crd::containers::Array<crd::math::Vec3<T>>& scratch)
{
    const crd::u32 pool = m.vertex_pool_size();
    scratch.resize(pool, crd::math::Vec3<T>{T{0}, T{0}, T{0}});

    crd::u32 moved = 0;
    for (crd::u32 v = 0; v < pool; ++v)
    {
        if (!m.vertex_alive(v))
        {
            scratch[v] = crd::math::Vec3<T>{T{0}, T{0}, T{0}};
            continue;
        }
        const auto& p = m.vertex(v).position;
        if (keep_boundary_fixed && is_boundary[v] != 0U)
        {
            scratch[v] = p;
            continue;
        }

        // Accumulate one-ring sum + count. For boundary vertices (when
        // NOT clamped), use ONLY boundary neighbours per the v7c
        // cubic-B-spline boundary-curve intent.
        crd::math::Vec3<T> sum{T{0}, T{0}, T{0}};
        crd::u32           n   = 0;
        const bool         vb  = is_boundary[v] != 0U;
        m.for_each_outgoing_he(v, [&](crd::u32 ho) {
            const crd::u32 dest = m.he_dest(ho);
            if (dest == k_null_vertex) { return; }
            if (vb)
            {
                // Only count boundary-edge neighbours.
                const bool       ho_is_b = m.he_is_boundary(ho);
                const crd::u32   t        = m.he(ho).twin;
                const bool       t_is_b   = (t != k_null_he) && m.he_is_boundary(t);
                if (!ho_is_b && !t_is_b) { return; }
            }
            sum = sum + m.vertex(dest).position;
            ++n;
        });
        if (n == 0U)
        {
            scratch[v] = p;
            continue;
        }
        const T               inv_n     = T{1} / static_cast<T>(n);
        const crd::math::Vec3<T> centroid{sum.x * inv_n, sum.y * inv_n, sum.z * inv_n};
        // L = Laplacian (centroid − p) per Taubin 1995 §2 notation.
        const crd::math::Vec3<T> L{centroid.x - p.x, centroid.y - p.y, centroid.z - p.z}; // NOLINT(readability-identifier-naming)
        scratch[v] = crd::math::Vec3<T>{p.x + factor * L.x,
                                          p.y + factor * L.y,
                                          p.z + factor * L.z};
        ++moved;
    }

    // Apply atomically.
    for (crd::u32 v = 0; v < pool; ++v)
    {
        if (!m.vertex_alive(v)) { continue; }
        m.set_vertex_position(v, scratch[v]);
    }
    return moved;
}

} // anonymous namespace

template <crd::math::MathScalar T>
HalfEdgeMesh<T> taubin_smooth(const HalfEdgeMesh<T>&             input,
                               const TaubinSmoothOptions<T>&     opts,
                               TaubinSmoothReport*               out_report)
{
    TaubinSmoothReport report{};
    auto                report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* alloc = opts.output_allocator != nullptr
                                          ? opts.output_allocator
                                          : input.allocator();
    CRD_ASSERT(alloc != nullptr);

    if (opts.lambda <= T{0} || opts.mu >= T{0})
    {
        report.status = TaubinSmoothStatus::InvalidParameters;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }
    if (input.face_count() == 0U)
    {
        report.status = TaubinSmoothStatus::EmptyMesh;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }
    if (!input.is_manifold())
    {
        report.status = TaubinSmoothStatus::NonManifoldInput;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }

    // Clone input to mutable output.
    HalfEdgeMesh<T>                            output{alloc};
    crd::containers::Array<crd::math::Vec3<T>> pos(alloc);
    crd::containers::Array<crd::u32>           idx(alloc);
    input.to_indexed(pos, idx);
    const auto bs = output.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{pos.data(), pos.size()},
        crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()});
    (void)bs;

    // Cache boundary flags once (D68).
    crd::containers::Array<crd::u8> is_boundary(alloc);
    is_boundary.resize(output.vertex_pool_size(), crd::u8{0});
    for (crd::u32 v = 0; v < output.vertex_pool_size(); ++v)
    {
        if (!output.vertex_alive(v)) { continue; }
        if (is_boundary_vertex(output, v))
        {
            is_boundary[v] = 1U;
            ++report.boundary_vertices_clamped;
        }
    }

    crd::containers::Array<crd::math::Vec3<T>> scratch(alloc);

    for (crd::u32 it = 0; it < opts.n_iterations; ++it)
    {
        // Pass 1: shrink with λ.
        report.vertices_smoothed += apply_taubin_pass(output, is_boundary, opts.lambda,
                                                       opts.keep_boundary_fixed, scratch);
        // Pass 2: un-shrink with μ.
        report.vertices_smoothed += apply_taubin_pass(output, is_boundary, opts.mu,
                                                       opts.keep_boundary_fixed, scratch);
        ++report.iterations_run;
    }

    report.output_vertices = output.vertex_count();
    report.output_faces    = output.face_count();
    report_out();
    return output;
}

template HalfEdgeMesh<crd::f32> taubin_smooth<crd::f32>(const HalfEdgeMesh<crd::f32>&,
                                                          const TaubinSmoothOptions<crd::f32>&,
                                                          TaubinSmoothReport*);
template HalfEdgeMesh<crd::f64> taubin_smooth<crd::f64>(const HalfEdgeMesh<crd::f64>&,
                                                          const TaubinSmoothOptions<crd::f64>&,
                                                          TaubinSmoothReport*);

} // namespace crd::geometry::mesh_processing
