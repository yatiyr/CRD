// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8g Ruppert 1995 2D Delaunay refinement.
//
// See ruppert_2d.hpp for algorithm contract. This TU owns:
//   - Input validation + alpha-range check.
//   - Per-iteration encroachment scan over current segment list.
//   - Per-iteration bad-triangle scan over current CDT.
//   - Segment-midpoint split / circumcentre Steiner insertion w/ encroach-
//     prioritisation (split encroached segment before inserting Steiner).
//   - Per-iteration full CDT rebuild via `crd::geometry::polygon::constrained_delaunay`
//     (simple-but-correct v1; incremental Bowyer-Watson update is a
//     v8g-perf follow-on per ADR-0076 §23 D~119).
//
// Pinned design decisions D114-D118 (carryover for ADR-0076 §23 at v8-close):
//
//   D114. **Encroachment test = diametral-disk inclusion via dot product**.
//         Segment (A, B) is encroached by V iff `dot(A - V, B - V) < 0`
//         (angle AVB obtuse / V inside the diametral disk). Equivalent
//         predicate; one multiply-add per check, no sqrt.
//
//   D115. **Encroach-first prioritisation**: each iteration, scan SEGMENTS
//         before triangles. Splitting an encroached segment can resolve
//         multiple bad triangles simultaneously, and a Steiner point at a
//         circumcentre that encroaches a segment must NOT be inserted —
//         instead split that segment first (Ruppert's correctness condition).
//
//   D116. **Min-angle calc via law-of-cosines** on squared side lengths.
//         `cos²(α) = ((b² + c² - a²) / (2bc))²` — avoids sqrt for the
//         comparison, but we use the sqrt form for clarity since the inner
//         loop is dominated by CDT rebuilds. Min angle = angle opposite the
//         SHORTEST side.
//
//   D117. **Full CDT rebuild per iteration** in v1. Simple, deterministic,
//         O(N log N) per rebuild × O(N) iterations = O(N² log N) total.
//         Acceptable for meshes up to ~10k vertices. Incremental
//         Bowyer-Watson + segment-protection cavity is a v8g-perf
//         follow-on slice.
//
//   D118. **Deterministic ordering**: scan segments in input-index order
//         (sub-segments inserted at end), scan triangles in CDT-output
//         order. First encroached segment found is split; first bad
//         triangle found has its circumcentre considered. Lex-tiebreak
//         determinism preserved across iterations.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/ruppert_2d.hpp>
#include <crd/geometry/polygon/cdt.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::geometry::delaunay
{

namespace
{

inline RuppertStatus propagate_cdt_status(crd::geometry::polygon::CdtStatus s) noexcept
{
    using crd::geometry::polygon::CdtStatus;
    switch (s)
    {
        case CdtStatus::Ok:                  return RuppertStatus::Ok;
        case CdtStatus::TooFewPoints:        return RuppertStatus::TooFewPoints;
        case CdtStatus::NonFiniteInput:      return RuppertStatus::NonFiniteInput;
        case CdtStatus::DuplicatePoint:      return RuppertStatus::DuplicatePoint;
        case CdtStatus::ConstraintOutOfBounds: return RuppertStatus::ConstraintOutOfBounds;
        case CdtStatus::ConstraintsCrossing: return RuppertStatus::ConstraintsCrossing;
        case CdtStatus::InternalInvariant:   return RuppertStatus::InternalInvariant;
    }
    return RuppertStatus::InternalInvariant;
}

template <crd::math::MathScalar T>
inline bool is_finite_vec2(const crd::math::Vec2<T>& p) noexcept
{
    return std::isfinite(static_cast<double>(p.x)) && std::isfinite(static_cast<double>(p.y));
}

// Encroachment test (D114): vertex V encroaches segment (A, B) iff
// dot(A - V, B - V) < 0. Optionally exclude V == A or V == B from the
// encroaching set.
template <crd::math::MathScalar T>
inline bool encroaches(const crd::math::Vec2<T>& a,
                        const crd::math::Vec2<T>& b,
                        const crd::math::Vec2<T>& v) noexcept
{
    const T avx = a.x - v.x;
    const T avy = a.y - v.y;
    const T bvx = b.x - v.x;
    const T bvy = b.y - v.y;
    return (avx * bvx + avy * bvy) < static_cast<T>(0);
}

// Check if a candidate point is within `eps_sq` of any existing vertex
// (squared distance). Used to skip Steiner insertions that would create
// near-duplicate points (CDT rejects exact duplicates via DuplicatePoint
// status). Returns true if duplicate-like.
template <crd::math::MathScalar T>
inline bool is_near_existing(const crd::containers::Array<crd::math::Vec2<T>>& vertices,
                               const crd::math::Vec2<T>& candidate,
                               T eps_sq) noexcept
{
    return std::ranges::any_of(vertices, [&](const crd::math::Vec2<T>& v) {
        const T dx = candidate.x - v.x;
        const T dy = candidate.y - v.y;
        return dx * dx + dy * dy < eps_sq;
    });
}

// Min angle of triangle (a, b, c) in radians via law of cosines.
template <crd::math::MathScalar T>
T triangle_min_angle_rad(const crd::math::Vec2<T>& a,
                          const crd::math::Vec2<T>& b,
                          const crd::math::Vec2<T>& c) noexcept
{
    const T abx = b.x - a.x, aby = b.y - a.y; // NOLINT(readability-isolate-declaration)
    const T acx = c.x - a.x, acy = c.y - a.y; // NOLINT(readability-isolate-declaration)
    const T bcx = c.x - b.x, bcy = c.y - b.y; // NOLINT(readability-isolate-declaration)
    const T len2_ab = abx * abx + aby * aby;
    const T len2_ac = acx * acx + acy * acy;
    const T len2_bc = bcx * bcx + bcy * bcy;
    if (len2_ab <= static_cast<T>(0) || len2_ac <= static_cast<T>(0) || len2_bc <= static_cast<T>(0))
    {
        return static_cast<T>(0);
    }
    const T len_ab = std::sqrt(len2_ab);
    const T len_ac = std::sqrt(len2_ac);
    const T len_bc = std::sqrt(len2_bc);
    // Cos of each angle via dot product. Angle at A = between AB and AC.
    auto safe_acos = [](T x) -> T {
        if (x > static_cast<T>(1))  { x = static_cast<T>(1); }
        if (x < static_cast<T>(-1)) { x = static_cast<T>(-1); }
        return std::acos(x);
    };
    const T cos_a = (abx * acx + aby * acy) / (len_ab * len_ac);
    const T cos_b = ((-abx) * bcx + (-aby) * bcy) / (len_ab * len_bc);
    const T cos_c = ((-acx) * (-bcx) + (-acy) * (-bcy)) / (len_ac * len_bc);
    const T ang_a = safe_acos(cos_a);
    const T ang_b = safe_acos(cos_b);
    const T ang_c = safe_acos(cos_c);
    T m = ang_a;
    if (ang_b < m) { m = ang_b; }
    if (ang_c < m) { m = ang_c; }
    return m;
}

// Run CDT on the current vertex + segment lists. Output triangle_indices.
template <crd::math::MathScalar T>
crd::geometry::polygon::CdtStatus
run_cdt(const crd::containers::Array<crd::math::Vec2<T>>& vertices,
         const crd::containers::Array<RuppertSegment>&     segments,
         crd::memory::IAllocator*                          alloc,
         crd::containers::Array<crd::u32>&                 out_tri_indices,
         crd::u32&                                          out_tri_count)
{
    // Convert RuppertSegment -> CdtEdge.
    crd::containers::Array<crd::geometry::polygon::CdtEdge> cdt_edges(alloc);
    cdt_edges.reserve(segments.size());
    for (const auto& s : segments)
    {
        crd::geometry::polygon::CdtEdge e{};
        e.a = s.a;
        e.b = s.b;
        cdt_edges.push_back(e);
    }
    auto cdt_res = crd::geometry::polygon::constrained_delaunay<T>(
        crd::containers::ConstSpan<crd::math::Vec2<T>>{vertices.data(), vertices.size()},
        crd::containers::ConstSpan<crd::geometry::polygon::CdtEdge>{cdt_edges.data(), cdt_edges.size()},
        alloc);
    if (!cdt_res.ok()) { return cdt_res.status; }
    out_tri_indices = std::move(cdt_res.triangle_indices);
    out_tri_count   = cdt_res.triangle_count;
    return crd::geometry::polygon::CdtStatus::Ok;
}

} // anonymous namespace

template <crd::math::MathScalar T>
RuppertResult2<T>
ruppert_refine_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                   crd::containers::ConstSpan<RuppertSegment>      segments,
                   const RuppertOptions<T>&                        opts,
                   crd::memory::IAllocator*                        alloc)
{
    RuppertResult2<T> result{alloc};
    const crd::u32 n_in = static_cast<crd::u32>(points.size());

    // Validate.
    if (n_in < 3U) { result.status = RuppertStatus::TooFewPoints; return result; }
    for (crd::u32 i = 0; i < n_in; ++i)
    {
        if (!is_finite_vec2(points[i])) { result.status = RuppertStatus::NonFiniteInput; return result; }
    }
    for (const auto& s : segments)
    {
        if (s.a >= n_in || s.b >= n_in)
        {
            result.status = RuppertStatus::ConstraintOutOfBounds;
            return result;
        }
    }
    if (opts.min_angle_degrees <= static_cast<T>(0) || opts.min_angle_degrees > static_cast<T>(60))
    {
        result.status = RuppertStatus::InvalidAngle;
        return result;
    }
    const T min_angle_rad = opts.min_angle_degrees * static_cast<T>(3.14159265358979323846 / 180.0);

    // Initialise working vertex + segment arrays from input.
    crd::containers::Array<crd::math::Vec2<T>> vertices(alloc);
    vertices.reserve(n_in);
    for (crd::u32 i = 0; i < n_in; ++i) { vertices.push_back(points[i]); }
    crd::containers::Array<RuppertSegment> segs(alloc);
    segs.reserve(segments.size());
    for (const auto& s : segments) { segs.push_back(s); }

    // Initial CDT.
    crd::containers::Array<crd::u32> tri_indices(alloc);
    crd::u32 tri_count = 0;
    {
        const auto s = run_cdt<T>(vertices, segs, alloc, tri_indices, tri_count);
        if (s != crd::geometry::polygon::CdtStatus::Ok)
        {
            result.status = propagate_cdt_status(s);
            return result;
        }
    }

    // Refinement loop.
    for (crd::u32 iter = 0; iter < opts.max_iterations; ++iter)
    {
        result.iterations_run = iter + 1U;

        // Step (a): find any encroached segment. Scan segments in order;
        // for each segment, check all current vertices.
        crd::u32 encroached_idx = std::numeric_limits<crd::u32>::max();
        for (crd::u32 si = 0; si < segs.size(); ++si)
        {
            const auto& seg = segs[si];
            const auto& pa = vertices[seg.a];
            const auto& pb = vertices[seg.b];
            for (crd::u32 vi = 0; vi < vertices.size(); ++vi)
            {
                if (vi == seg.a || vi == seg.b) { continue; }
                if (encroaches<T>(pa, pb, vertices[vi]))
                {
                    encroached_idx = si;
                    break;
                }
            }
            if (encroached_idx != std::numeric_limits<crd::u32>::max()) { break; }
        }

        if (encroached_idx != std::numeric_limits<crd::u32>::max())
        {
            // Split encroached segment at midpoint.
            const auto seg = segs[encroached_idx];
            const auto& pa = vertices[seg.a];
            const auto& pb = vertices[seg.b];
            const crd::math::Vec2<T> midpoint{
                (pa.x + pb.x) * static_cast<T>(0.5),
                (pa.y + pb.y) * static_cast<T>(0.5)
            };
            const T eps_sq_split = static_cast<T>(1e-12);
            if (is_near_existing<T>(vertices, midpoint, eps_sq_split))
            {
                // Segment is too short to split further (numerical limit).
                // Bail out to avoid duplicate-point error.
                break;
            }
            const crd::u32 mid_idx = static_cast<crd::u32>(vertices.size());
            vertices.push_back(midpoint);
            ++result.steiner_count;
            if (result.steiner_count > opts.max_steiner)
            {
                break; // give up, NotConverged
            }
            // Replace segs[encroached_idx] = (a, mid); push new segment
            // (mid, b) at end.
            segs[encroached_idx] = RuppertSegment{seg.a, mid_idx};
            segs.push_back(RuppertSegment{mid_idx, seg.b});
            // Re-CDT.
            const auto s = run_cdt<T>(vertices, segs, alloc, tri_indices, tri_count);
            if (s != crd::geometry::polygon::CdtStatus::Ok)
            {
                result.status = propagate_cdt_status(s);
                return result;
            }
            continue;
        }

        // Step (b): find any bad triangle (min-angle < alpha).
        crd::u32 bad_tri = std::numeric_limits<crd::u32>::max();
        T bad_min_angle = static_cast<T>(0);
        for (crd::u32 t = 0; t < tri_count; ++t)
        {
            const crd::u32 ia = tri_indices[3U * t + 0U];
            const crd::u32 ib = tri_indices[3U * t + 1U];
            const crd::u32 ic = tri_indices[3U * t + 2U];
            const T ang = triangle_min_angle_rad<T>(vertices[ia], vertices[ib], vertices[ic]);
            if (ang < min_angle_rad)
            {
                bad_tri = t;
                bad_min_angle = ang;
                break;
            }
        }

        if (bad_tri == std::numeric_limits<crd::u32>::max())
        {
            // Converged.
            result.converged = true;
            result.status    = RuppertStatus::Ok;
            break;
        }

        // Compute circumcentre.
        const crd::u32 ia = tri_indices[3U * bad_tri + 0U];
        const crd::u32 ib = tri_indices[3U * bad_tri + 1U];
        const crd::u32 ic = tri_indices[3U * bad_tri + 2U];
        const auto cc = crd::geometry::primitives::circumcenter_2d(
            vertices[ia], vertices[ib], vertices[ic]);
        (void)bad_min_angle;

        // Encroachment check: does cc encroach any segment? If yes, split
        // the encroached segment first (Ruppert's correctness condition).
        crd::u32 encroach_by_cc = std::numeric_limits<crd::u32>::max();
        for (crd::u32 si = 0; si < segs.size(); ++si)
        {
            const auto& seg = segs[si];
            const auto& pa = vertices[seg.a];
            const auto& pb = vertices[seg.b];
            if (encroaches<T>(pa, pb, cc))
            {
                encroach_by_cc = si;
                break;
            }
        }

        if (encroach_by_cc != std::numeric_limits<crd::u32>::max())
        {
            // Split encroached segment instead of inserting circumcentre.
            const auto seg = segs[encroach_by_cc];
            const auto& pa = vertices[seg.a];
            const auto& pb = vertices[seg.b];
            const crd::math::Vec2<T> midpoint{
                (pa.x + pb.x) * static_cast<T>(0.5),
                (pa.y + pb.y) * static_cast<T>(0.5)
            };
            const T eps_sq_split = static_cast<T>(1e-12);
            if (is_near_existing<T>(vertices, midpoint, eps_sq_split))
            {
                break; // cannot split further
            }
            const crd::u32 mid_idx = static_cast<crd::u32>(vertices.size());
            vertices.push_back(midpoint);
            ++result.steiner_count;
            if (result.steiner_count > opts.max_steiner) { break; }
            segs[encroach_by_cc] = RuppertSegment{seg.a, mid_idx};
            segs.push_back(RuppertSegment{mid_idx, seg.b});
            const auto s = run_cdt<T>(vertices, segs, alloc, tri_indices, tri_count);
            if (s != crd::geometry::polygon::CdtStatus::Ok)
            {
                result.status = propagate_cdt_status(s);
                return result;
            }
            continue;
        }

        // Insert circumcentre as Steiner vertex. Defensive: skip if it
        // coincides (within numerical eps) with any existing vertex — CDT
        // rejects exact duplicates, and near-duplicates produce degenerate
        // sliver triangles that can't be refined further. Treat as
        // unrefinable and stop (NotConverged).
        const T eps_sq = static_cast<T>(1e-12);
        if (is_near_existing<T>(vertices, cc, eps_sq))
        {
            // Cannot refine further on this triangle — bail.
            break;
        }
        vertices.push_back(cc);
        ++result.steiner_count;
        if (result.steiner_count > opts.max_steiner) { break; }
        const auto s = run_cdt<T>(vertices, segs, alloc, tri_indices, tri_count);
        if (s != crd::geometry::polygon::CdtStatus::Ok)
        {
            result.status = propagate_cdt_status(s);
            return result;
        }
    }

    // Emit final result.
    if (!result.converged && result.status == RuppertStatus::Ok)
    {
        result.status = RuppertStatus::NotConverged;
    }
    result.vertices         = std::move(vertices);
    result.triangle_indices = std::move(tri_indices);
    result.refined_segments = std::move(segs);
    result.triangle_count   = tri_count;
    return result;
}

// Explicit instantiations.
template RuppertResult2<crd::f32>
ruppert_refine_2d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>,
                              crd::containers::ConstSpan<RuppertSegment>,
                              const RuppertOptions<crd::f32>&, crd::memory::IAllocator*);
template RuppertResult2<crd::f64>
ruppert_refine_2d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>,
                              crd::containers::ConstSpan<RuppertSegment>,
                              const RuppertOptions<crd::f64>&, crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
