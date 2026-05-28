// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7e + v7e-refine Liepa 2003 hole filling.
//
// See header for the algorithm contract. Three phases:
//
//   PHASE §3 (DP)         min-weight Liepa triangulation of each boundary
//                          loop (this TU's `dp_compute` + `reconstruct_
//                          triangulation`). Always runs.
//
//   PHASE §4 (Refinement)  Steiner-point insertion at too-coarse triangle
//                          centroids + Delaunay-flip pass on patch interior
//                          edges. Iterates until convergence (no splits)
//                          or `max_refine_iterations`. Skips when
//                          `opts.refine == false`.
//
//   PHASE §5 (Fairing)     Jacobi-style Laplacian smoothing of Steiner
//                          vertices (loop vertices CLAMPED). Iterates
//                          `opts.fairing_iterations` times. No-op when
//                          there are no Steiner points or
//                          `fairing_iterations == 0`.
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D39-D45 — see initial v7e session log + first version of this TU.
//
//   D46. **§4 refinement implemented at INDEX LEVEL with temporary
//        HalfEdgeMesh rebuilt per flip pass.** Splits update
//        `patch_positions`, `patch_sigma`, `patch_indices` directly
//        (replace 1 triangle with 3, append 1 Steiner vertex). Flips
//        build a temp HalfEdgeMesh from `patch_indices`, run
//        `flip_edge` on every interior edge that meets the Delaunay
//        criterion, extract back to `patch_indices`. Avoids needing
//        a new `HalfEdgeMesh::split_face_centroid` atomic op.
//
//   D47. **σ scale per loop vertex** = arithmetic mean of incident
//        edge lengths in the INPUT mesh (walk `input.for_each_outgoing_he`,
//        sum `length(v, dest)`, divide by count). σ for Steiner
//        vertices set at creation = arithmetic mean of the parent
//        triangle's three σ values (Liepa 2003 §4 propagation rule).
//
//   D48. **Too-coarse test** (Liepa 2003 §4.1): triangle T = (a, b, c)
//        is too coarse iff for SOME vertex v ∈ T,
//          α · |v - centroid(T)| > σ_v   AND
//          α · |v - centroid(T)| > σ_avg
//        where α = √2 by default (configurable). Squared form: 2 ·
//        |v - c|² > σ_v² AND 2 · |v - c|² > σ_avg². Boundary loop
//        vertices' σ is the INPUT-mesh σ; Steiner-point σ is their
//        creation-time σ_avg.
//
//   D49. **Delaunay flip criterion**: edge (a, b) with apex vertices
//        c, d (from the two adjacent patch triangles) is flipped iff
//        ∠acb + ∠adb > π. Computed via `crd::math::deterministic::acos`
//        on the unit-length-clamped cosine. Loop boundary edges
//        (= boundary in the temp patch-only HE mesh) are auto-skipped
//        by HalfEdgeMesh::flip_edge's gate. Flip duplicate-edge gate
//        (`vertices_connected`, same fix as v7d D36) prevents creating
//        non-manifold patches.
//
//   D50. **§5 Laplacian fairing** uses Jacobi update: each Steiner
//        vertex's new position = arithmetic mean of its 1-ring neighbour
//        positions in the patch. All new positions computed against
//        OLD; applied atomically via `set_vertex_position` (same
//        pattern as v7d tangential smoothing). Loop vertices skip the
//        update — boundary clamp.
//
//   D51. **Local-to-global vertex remap** for Steiner appendage: at
//        emission time, snapshot `first_steiner_global = global_
//        positions.size()`, append all Steiner positions, then map
//        each local patch-vertex index `local`:
//          if local < loop_size  →  loop_global_indices[local]
//          else                  →  first_steiner_global + (local -
//                                                          loop_size)
//        Maintains stable global vertex identity for both loop
//        vertices (unchanged) and Steiner vertices (new, sequential).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/fill_holes.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::geometry::mesh_processing
{
namespace
{

inline crd::usize ix(crd::u32 i, crd::u32 k, crd::u32 n) noexcept
{
    return static_cast<crd::usize>(i) * static_cast<crd::usize>(n) + static_cast<crd::usize>(k);
}

template <crd::math::MathScalar T>
crd::math::Vec3<T> triangle_normal(const crd::math::Vec3<T>& a,
                                    const crd::math::Vec3<T>& b,
                                    const crd::math::Vec3<T>& c) noexcept
{
    const auto n = crd::math::cross(b - a, c - a);
    const T    len = crd::math::length(n);
    if (len < static_cast<T>(1e-20)) { return crd::math::Vec3<T>{T{0}, T{0}, T{0}}; }
    return n * (T{1} / len);
}

template <crd::math::MathScalar T>
T triangle_area(const crd::math::Vec3<T>& a,
                 const crd::math::Vec3<T>& b,
                 const crd::math::Vec3<T>& c) noexcept
{
    return crd::math::length(crd::math::cross(b - a, c - a)) * T{0.5};
}

template <crd::math::MathScalar T>
T dihedral_penalty(const crd::math::Vec3<T>& n1,
                    const crd::math::Vec3<T>& n2) noexcept
{
    const T d = crd::math::dot(n1, n2);
    const T clamped = std::clamp(d, T{-1}, T{1});
    return T{1} - clamped;
}

template <crd::math::MathScalar T>
T edge_length(const crd::math::Vec3<T>& a, const crd::math::Vec3<T>& b) noexcept
{
    const T dx = b.x - a.x;
    const T dy = b.y - a.y;
    const T dz = b.z - a.z;
    return static_cast<T>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
}

// Detect all boundary loops; each loop is a list of vertex indices in
// walk order via boundary HE `.next`.
template <crd::math::MathScalar T>
void detect_boundary_loops(const HalfEdgeMesh<T>&                              m,
                            crd::memory::IAllocator*                            alloc,
                            crd::containers::Array<crd::containers::Array<crd::u32>>& out_loops)
{
    crd::containers::Array<crd::u8> visited(alloc);
    visited.resize(m.he_pool_size(), crd::u8{0});

    for (crd::u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        if (!m.he_is_boundary(h)) { continue; }
        if (visited[h] != 0U) { continue; }

        crd::containers::Array<crd::u32> loop(alloc);
        crd::u32 cur = h;
        const crd::u32 cap = m.he_pool_size() + 4U;
        for (crd::u32 step = 0; step < cap; ++step)
        {
            visited[cur] = 1U;
            loop.push_back(m.he(cur).origin);
            const crd::u32 nxt = m.he(cur).next;
            if (nxt == k_null_he) { break; }
            if (nxt == h) { break; }
            cur = nxt;
        }
        if (loop.size() >= 3U) { out_loops.push_back(std::move(loop)); }
    }
}

// Per loop vertex, find the boundary HE going (b_i → b_{i+1}); compute
// the outside-mesh face normal from its twin's face.
template <crd::math::MathScalar T>
void precompute_outside_normals(const HalfEdgeMesh<T>&                  m,
                                  const crd::containers::Array<crd::u32>& loop,
                                  crd::containers::Array<crd::math::Vec3<T>>& out_normals)
{
    // N = loop vertex count per Liepa 1997 §3 DP-table notation.
    const crd::u32 N = static_cast<crd::u32>(loop.size()); // NOLINT(readability-identifier-naming)
    out_normals.resize(N, crd::math::Vec3<T>{T{0}, T{0}, T{0}});
    for (crd::u32 i = 0; i < N; ++i)
    {
        const crd::u32 v_a = loop[i];
        const crd::u32 v_b = loop[(i + 1U) % N];
        crd::u32       boundary_h = k_null_he;
        m.for_each_outgoing_he(v_a, [&](crd::u32 ho) {
            if (boundary_h != k_null_he) { return; }
            if (!m.he_is_boundary(ho)) { return; }
            if (m.he_dest(ho) == v_b) { boundary_h = ho; }
        });
        if (boundary_h == k_null_he) { continue; }
        const crd::u32 t = m.he(boundary_h).twin;
        if (t == k_null_he) { continue; }
        const crd::u32 f = m.he(t).face;
        if (f == k_null_face) { continue; }
        const crd::u32 h0 = m.face(f).first_he;
        const crd::u32 h1 = m.he(h0).next;
        const crd::u32 h2 = m.he(h1).next;
        const auto&    p0 = m.vertex(m.he(h0).origin).position;
        const auto&    p1 = m.vertex(m.he(h1).origin).position;
        const auto&    p2 = m.vertex(m.he(h2).origin).position;
        out_normals[i] = triangle_normal(p0, p1, p2);
    }
}

template <crd::math::MathScalar T>
void dp_compute(const crd::containers::Array<crd::math::Vec3<T>>& loop_positions,
                 const crd::containers::Array<crd::math::Vec3<T>>& outside_normals,
                 T                                                  dihedral_lambda,
                 crd::containers::Array<T>&                         W_area,
                 crd::containers::Array<T>&                         W_dihedral,
                 crd::containers::Array<crd::u32>&                  O)
{
    // N = loop vertex count per Liepa 1997 §3 DP-table notation.
    const crd::u32 N = static_cast<crd::u32>(loop_positions.size()); // NOLINT(readability-identifier-naming)
    W_area.resize(static_cast<crd::usize>(N) * N, T{0});
    W_dihedral.resize(static_cast<crd::usize>(N) * N, T{0});
    O.resize(static_cast<crd::usize>(N) * N, crd::u32{0});

    for (crd::u32 span = 2; span < N; ++span)
    {
        for (crd::u32 i = 0; i + span < N; ++i)
        {
            const crd::u32 k = i + span;
            T  best_composite = std::numeric_limits<T>::infinity();
            T  best_area      = T{0};
            T  best_dihedral  = T{0};
            crd::u32 best_m   = i + 1U;
            for (crd::u32 m = i + 1U; m < k; ++m)
            {
                const auto& pi = loop_positions[i];
                const auto& pm = loop_positions[m];
                const auto& pk = loop_positions[k];
                // area_T / n_T = triangle area + normal per Liepa 1997 §3 notation.
                const T     area_T = triangle_area(pi, pm, pk);     // NOLINT(readability-identifier-naming)
                const auto  n_T    = triangle_normal(pi, pm, pk);   // NOLINT(readability-identifier-naming)

                T dih_im = T{0};
                if (m == i + 1U)
                {
                    dih_im = dihedral_penalty(n_T, outside_normals[i]);
                }
                else
                {
                    const crd::u32 pm_split = O[ix(i, m, N)];
                    const auto     n_adj = triangle_normal(loop_positions[i],
                                                              loop_positions[pm_split],
                                                              loop_positions[m]);
                    dih_im = dihedral_penalty(n_T, n_adj);
                }

                T dih_mk = T{0};
                if (k == m + 1U)
                {
                    dih_mk = dihedral_penalty(n_T, outside_normals[m]);
                }
                else
                {
                    const crd::u32 mk_split = O[ix(m, k, N)];
                    const auto     n_adj = triangle_normal(loop_positions[m],
                                                              loop_positions[mk_split],
                                                              loop_positions[k]);
                    dih_mk = dihedral_penalty(n_T, n_adj);
                }

                const T sub_area = W_area[ix(i, m, N)] + W_area[ix(m, k, N)];
                const T sub_dih  = W_dihedral[ix(i, m, N)] + W_dihedral[ix(m, k, N)];
                const T total_area = sub_area + area_T;
                const T total_dih  = sub_dih + dih_im + dih_mk;
                const T composite  = total_area + dihedral_lambda * total_dih;
                if (composite < best_composite)
                {
                    best_composite = composite;
                    best_area      = total_area;
                    best_dihedral  = total_dih;
                    best_m         = m;
                }
            }
            W_area[ix(i, k, N)]     = best_area;
            W_dihedral[ix(i, k, N)] = best_dihedral;
            O[ix(i, k, N)]          = best_m;
        }
    }
}

void reconstruct_triangulation(const crd::containers::Array<crd::u32>& O,
                                crd::u32                                N,
                                crd::u32                                i,
                                crd::u32                                k,
                                crd::containers::Array<crd::u32>&       out_local_idx)
{
    if (k <= i + 1U) { return; }
    const crd::u32 m = O[ix(i, k, N)];
    out_local_idx.push_back(i);
    out_local_idx.push_back(m);
    out_local_idx.push_back(k);
    reconstruct_triangulation(O, N, i, m, out_local_idx);
    reconstruct_triangulation(O, N, m, k, out_local_idx);
}

// Compute σ_v for each loop vertex from the INPUT mesh: arithmetic mean
// of incident edge lengths.
template <crd::math::MathScalar T>
void compute_loop_sigma(const HalfEdgeMesh<T>&                  input,
                         const crd::containers::Array<crd::u32>& loop_global_indices,
                         crd::containers::Array<T>&              out_sigma)
{
    // N = loop vertex count per Liepa 1997 §3 notation.
    const crd::u32 N = static_cast<crd::u32>(loop_global_indices.size()); // NOLINT(readability-identifier-naming)
    out_sigma.resize(N, T{0});
    for (crd::u32 i = 0; i < N; ++i)
    {
        const crd::u32 v = loop_global_indices[i];
        const auto&    p = input.vertex(v).position;
        T              sum_len = T{0};
        crd::u32       count   = 0;
        input.for_each_outgoing_he(v, [&](crd::u32 ho) {
            const crd::u32 dest = input.he_dest(ho);
            if (dest == k_null_vertex) { return; }
            sum_len += edge_length(p, input.vertex(dest).position);
            ++count;
        });
        out_sigma[i] = count > 0U ? sum_len / static_cast<T>(count) : T{0};
    }
}

// Liepa §4 too-coarse test: returns true iff triangle (a, b, c) needs
// splitting given σ values and α (default √2).
template <crd::math::MathScalar T>
bool too_coarse(const crd::math::Vec3<T>& pa, T sigma_a,
                 const crd::math::Vec3<T>& pb, T sigma_b,
                 const crd::math::Vec3<T>& pc, T sigma_c,
                 T alpha) noexcept
{
    const crd::math::Vec3<T> centroid{
        (pa.x + pb.x + pc.x) / T{3},
        (pa.y + pb.y + pc.y) / T{3},
        (pa.z + pb.z + pc.z) / T{3},
    };
    const T sigma_avg = (sigma_a + sigma_b + sigma_c) / T{3};
    const T sigma_avg_sq = sigma_avg * sigma_avg;
    const T alpha_sq = alpha * alpha;
    auto dist_sq = [](const crd::math::Vec3<T>& u, const crd::math::Vec3<T>& v) {
        const T dx = u.x - v.x;
        const T dy = u.y - v.y;
        const T dz = u.z - v.z;
        return dx * dx + dy * dy + dz * dz;
    };
    // Triangle is too coarse iff for SOME vertex v ∈ T, BOTH
    //   α² · |v - c|² > σ_v²
    //   α² · |v - c|² > σ_avg²
    auto check_vertex = [&](const crd::math::Vec3<T>& v, T sigma_v) {
        const T d_sq = dist_sq(v, centroid);
        const T scaled = alpha_sq * d_sq;
        return scaled > sigma_v * sigma_v && scaled > sigma_avg_sq;
    };
    return check_vertex(pa, sigma_a) || check_vertex(pb, sigma_b) || check_vertex(pc, sigma_c);
}

// Delaunay-flip criterion: returns true iff ∠acb + ∠adb > π.
template <crd::math::MathScalar T>
bool delaunay_flip_recommended(const crd::math::Vec3<T>& pa,
                                 const crd::math::Vec3<T>& pb,
                                 const crd::math::Vec3<T>& pc,
                                 const crd::math::Vec3<T>& pd) noexcept
{
    auto angle_at = [](const crd::math::Vec3<T>& vert,
                        const crd::math::Vec3<T>& other_a,
                        const crd::math::Vec3<T>& other_b) -> T {
        const auto u = other_a - vert;
        const auto v = other_b - vert;
        const T    lu = crd::math::length(u);
        const T    lv = crd::math::length(v);
        if (lu < static_cast<T>(1e-20) || lv < static_cast<T>(1e-20)) { return T{0}; }
        T cos_v = crd::math::dot(u, v) / (lu * lv);
        if (cos_v > T{1}) { cos_v = T{1}; }
        if (cos_v < T{-1}) { cos_v = T{-1}; }
        return crd::math::deterministic::acos(cos_v);
    };
    const T angle_c = angle_at(pc, pa, pb);
    const T angle_d = angle_at(pd, pa, pb);
    constexpr T k_pi = static_cast<T>(3.14159265358979323846);
    return angle_c + angle_d > k_pi;
}

// Build temp HE mesh from local patch arrays; flip every interior edge
// whose Delaunay criterion is met AND whose flip would not create a
// duplicate edge (D36 from v7d). Returns flip count + extracts
// resulting indices back to `patch_indices`.
template <crd::math::MathScalar T>
crd::u32 delaunay_flip_pass(crd::containers::Array<crd::math::Vec3<T>>& patch_positions,
                             crd::containers::Array<crd::u32>&            patch_indices,
                             crd::memory::IAllocator*                     alloc)
{
    HalfEdgeMesh<T> temp{alloc};
    const auto bs = temp.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{patch_positions.data(), patch_positions.size()},
        crd::containers::ConstSpan<crd::u32>{patch_indices.data(), patch_indices.size()});
    (void)bs;
    if (!temp.is_manifold()) { return 0; } // bail; defensive

    // Snapshot canonical interior HEs.
    crd::containers::Array<crd::u32> snap(alloc);
    for (crd::u32 h = 0; h < temp.he_pool_size(); ++h)
    {
        if (!temp.he_alive(h)) { continue; }
        if (temp.he_is_boundary(h)) { continue; }
        const crd::u32 t = temp.he(h).twin;
        if (t == k_null_he || temp.he_is_boundary(t)) { continue; }
        if (h > t) { continue; }                  // canonical = smaller
        snap.push_back(h);
    }

    auto vertices_connected = [&](crd::u32 u, crd::u32 w) {
        bool found = false;
        temp.for_each_outgoing_he(u, [&](crd::u32 ho) {
            if (found) { return; }
            if (temp.he_dest(ho) == w) { found = true; }
        });
        return found;
    };

    crd::u32 flips = 0;
    for (crd::u32 si = 0; si < snap.size(); ++si)
    {
        const crd::u32 h = snap[si];
        if (!temp.he_alive(h)) { continue; }
        if (temp.he_is_boundary(h)) { continue; }
        const crd::u32 t = temp.he(h).twin;
        if (t == k_null_he || temp.he_is_boundary(t)) { continue; }
        const crd::u32 va = temp.he(h).origin;
        const crd::u32 vb = temp.he_dest(h);
        const crd::u32 vc = temp.he(temp.he_prev(h)).origin;
        const crd::u32 vd = temp.he(temp.he_prev(t)).origin;
        if (vertices_connected(vc, vd)) { continue; } // D36 guard
        if (!delaunay_flip_recommended(patch_positions[va], patch_positions[vb],
                                         patch_positions[vc], patch_positions[vd]))
        {
            continue;
        }
        if (temp.flip_edge(h)) { ++flips; }
    }

    if (flips > 0)
    {
        // Extract back to indexed form.
        crd::containers::Array<crd::math::Vec3<T>> new_positions(alloc);
        crd::containers::Array<crd::u32>           new_indices(alloc);
        crd::containers::Array<crd::u32>           old_to_new(alloc);
        temp.to_indexed(new_positions, new_indices, &old_to_new);
        // We expect topology-preserving flips ⇒ vertex count unchanged ⇒
        // positions identical.
        patch_indices = std::move(new_indices);
    }
    return flips;
}

// Liepa §4 refinement loop. Mutates patch_positions / patch_sigma /
// patch_indices in place. Loop_size = first patch_positions.size() that
// is a loop vertex (= constant). Returns total splits applied + total
// flips applied via out params.
template <crd::math::MathScalar T>
void refinement_loop(crd::containers::Array<crd::math::Vec3<T>>& patch_positions,
                      crd::containers::Array<T>&                   patch_sigma,
                      crd::containers::Array<crd::u32>&            patch_indices,
                      crd::u32                                     loop_size,
                      T                                            alpha,
                      crd::u32                                     max_iterations,
                      crd::memory::IAllocator*                     alloc,
                      crd::u32&                                    out_splits,
                      crd::u32&                                    out_flips,
                      crd::u32&                                    out_iterations)
{
    out_splits     = 0;
    out_flips      = 0;
    out_iterations = 0;
    for (crd::u32 iter = 0; iter < max_iterations; ++iter)
    {
        // Pass 1: per-triangle too-coarse check. Build new index buffer
        // with too-coarse triangles replaced by 3 sub-triangles each.
        crd::containers::Array<crd::u32> new_indices(alloc);
        new_indices.reserve(patch_indices.size());
        crd::u32 splits_this_iter = 0;
        for (crd::u32 ti = 0; ti + 2 < patch_indices.size(); ti += 3)
        {
            const crd::u32 va = patch_indices[ti + 0];
            const crd::u32 vb = patch_indices[ti + 1];
            const crd::u32 vc = patch_indices[ti + 2];
            const auto&    pa = patch_positions[va];
            const auto&    pb = patch_positions[vb];
            const auto&    pc = patch_positions[vc];
            const T        sa = patch_sigma[va];
            const T        sb = patch_sigma[vb];
            const T        sc = patch_sigma[vc];
            if (!too_coarse(pa, sa, pb, sb, pc, sc, alpha))
            {
                new_indices.push_back(va);
                new_indices.push_back(vb);
                new_indices.push_back(vc);
                continue;
            }
            // Split at centroid.
            const crd::math::Vec3<T> centroid{
                (pa.x + pb.x + pc.x) / T{3},
                (pa.y + pb.y + pc.y) / T{3},
                (pa.z + pb.z + pc.z) / T{3},
            };
            const T sigma_m = (sa + sb + sc) / T{3};
            const crd::u32 vm = static_cast<crd::u32>(patch_positions.size());
            patch_positions.push_back(centroid);
            patch_sigma.push_back(sigma_m);
            // Replace T_{a,b,c} with T_{a,b,m}, T_{b,c,m}, T_{c,a,m}
            // — CCW preserved (m is the same side as c was).
            new_indices.push_back(va);
            new_indices.push_back(vb);
            new_indices.push_back(vm);
            new_indices.push_back(vb);
            new_indices.push_back(vc);
            new_indices.push_back(vm);
            new_indices.push_back(vc);
            new_indices.push_back(va);
            new_indices.push_back(vm);
            ++splits_this_iter;
        }
        patch_indices = std::move(new_indices);
        out_splits += splits_this_iter;

        // Pass 2: Delaunay flips on interior edges. Builds a temp HE
        // mesh, flips, extracts back.
        const crd::u32 flips_this_iter = delaunay_flip_pass(patch_positions, patch_indices, alloc);
        out_flips += flips_this_iter;
        ++out_iterations;

        // Convergence: if no splits AND no flips happened, we're done.
        if (splits_this_iter == 0U && flips_this_iter == 0U) { break; }
    }
    // Suppress unused-param lint (alpha is used inside the lambda).
    (void)loop_size;
}

// Liepa §5 fairing: Laplacian smoothing of Steiner vertices only.
template <crd::math::MathScalar T>
crd::u32 fairing_pass(crd::containers::Array<crd::math::Vec3<T>>& patch_positions,
                       const crd::containers::Array<crd::u32>&     patch_indices,
                       crd::u32                                    loop_size,
                       crd::u32                                    iterations,
                       crd::memory::IAllocator*                    alloc)
{
    if (iterations == 0U) { return 0; }
    if (patch_positions.size() <= loop_size) { return 0; } // no Steiner vertices

    // Build adjacency once (patch topology doesn't change in fairing).
    HalfEdgeMesh<T> temp{alloc};
    const auto bs = temp.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{patch_positions.data(), patch_positions.size()},
        crd::containers::ConstSpan<crd::u32>{patch_indices.data(), patch_indices.size()});
    (void)bs;
    if (!temp.is_manifold()) { return 0; }

    crd::containers::Array<crd::math::Vec3<T>> new_positions(alloc);
    new_positions.resize(patch_positions.size(), crd::math::Vec3<T>{T{0}, T{0}, T{0}});

    crd::u32 iters_run = 0;
    for (crd::u32 iter = 0; iter < iterations; ++iter)
    {
        // Jacobi pass: compute new positions against OLD.
        for (crd::u32 v = 0; v < patch_positions.size(); ++v)
        {
            if (v < loop_size)
            {
                new_positions[v] = patch_positions[v]; // clamped boundary
                continue;
            }
            crd::math::Vec3<T> sum{T{0}, T{0}, T{0}};
            crd::u32           count = 0;
            temp.for_each_outgoing_he(v, [&](crd::u32 ho) {
                const crd::u32 dest = temp.he_dest(ho);
                if (dest == k_null_vertex) { return; }
                sum = sum + patch_positions[dest];
                ++count;
            });
            if (count > 0U)
            {
                new_positions[v] = sum * (T{1} / static_cast<T>(count));
            }
            else
            {
                new_positions[v] = patch_positions[v];
            }
        }
        // Apply.
        for (crd::u32 v = loop_size; v < patch_positions.size(); ++v)
        {
            patch_positions[v] = new_positions[v];
        }
        ++iters_run;
    }
    return iters_run;
}

} // anonymous namespace

template <crd::math::MathScalar T>
HalfEdgeMesh<T> fill_holes(const HalfEdgeMesh<T>&        input,
                            const FillHolesOptions<T>&   opts,
                            FillHolesReport*             out_report)
{
    FillHolesReport report{};
    auto            report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* alloc = opts.output_allocator != nullptr
                                          ? opts.output_allocator
                                          : input.allocator();
    CRD_ASSERT(alloc != nullptr);

    if (input.face_count() == 0U)
    {
        report.status = FillHolesStatus::EmptyMesh;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }
    if (!input.is_manifold())
    {
        report.status = FillHolesStatus::NonManifoldInput;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }

    crd::containers::Array<crd::containers::Array<crd::u32>> loops(alloc);
    detect_boundary_loops(input, alloc, loops);
    report.holes_detected = static_cast<crd::u32>(loops.size());

    crd::containers::Array<crd::math::Vec3<T>> global_positions(alloc);
    crd::containers::Array<crd::u32>           global_indices(alloc);
    input.to_indexed(global_positions, global_indices);

    for (crd::u32 li = 0; li < loops.size(); ++li)
    {
        const auto&    loop_globals = loops[li];
        // N = loop vertex count per Liepa 1997 §3 notation.
        const crd::u32 N            = static_cast<crd::u32>(loop_globals.size()); // NOLINT(readability-identifier-naming)
        if (N < 3U) { continue; }
        if (N > opts.max_hole_size)
        {
            ++report.holes_skipped_too_large;
            continue;
        }

        // §3 setup: outside normals + loop positions.
        crd::containers::Array<crd::math::Vec3<T>> outside_normals(alloc);
        precompute_outside_normals(input, loop_globals, outside_normals);

        crd::containers::Array<crd::math::Vec3<T>> patch_positions(alloc);
        patch_positions.reserve(N);
        for (crd::u32 vi = 0; vi < N; ++vi)
        {
            patch_positions.push_back(global_positions[loop_globals[vi]]);
        }

        // §3 DP. W_area / W_dihedral / O are the Liepa 1997 §3 DP tables.
        crd::containers::Array<T>        W_area(alloc);     // NOLINT(readability-identifier-naming)
        crd::containers::Array<T>        W_dihedral(alloc); // NOLINT(readability-identifier-naming)
        crd::containers::Array<crd::u32> O(alloc);          // NOLINT(readability-identifier-naming)
        dp_compute(patch_positions, outside_normals, opts.dihedral_lambda, W_area, W_dihedral, O);

        // §3 reconstruct: produces LOCAL indices (0..N-1) into patch_positions.
        crd::containers::Array<crd::u32> patch_indices(alloc);
        reconstruct_triangulation(O, N, 0U, N - 1U, patch_indices);

        // §4 refinement (optional).
        crd::containers::Array<T> patch_sigma(alloc);
        if (opts.refine)
        {
            compute_loop_sigma(input, loop_globals, patch_sigma);
            crd::u32 splits = 0;
            crd::u32 flips  = 0;
            crd::u32 iters  = 0;
            refinement_loop(patch_positions, patch_sigma, patch_indices, N,
                            opts.refine_alpha, opts.max_refine_iterations,
                            alloc, splits, flips, iters);
            report.steiner_points_added   += splits;
            report.delaunay_flips_applied += flips;
            report.refine_iterations_run  += iters;
        }

        // §5 fairing (optional; no-op without Steiner points).
        if (opts.fairing_iterations > 0U && patch_positions.size() > N)
        {
            report.fairing_iterations_run += fairing_pass(
                patch_positions, patch_indices, N, opts.fairing_iterations, alloc);
        }

        // Append Steiner vertices to global positions.
        const crd::u32 first_steiner_global = static_cast<crd::u32>(global_positions.size());
        for (crd::u32 vi = N; vi < patch_positions.size(); ++vi)
        {
            global_positions.push_back(patch_positions[vi]);
        }

        // Map local patch indices → global, append to global index buffer.
        auto local_to_global = [&](crd::u32 local) -> crd::u32 {
            if (local < N) { return loop_globals[local]; }
            return first_steiner_global + (local - N);
        };
        for (crd::u32 ti = 0; ti + 2 < patch_indices.size(); ti += 3)
        {
            global_indices.push_back(local_to_global(patch_indices[ti + 0]));
            global_indices.push_back(local_to_global(patch_indices[ti + 1]));
            global_indices.push_back(local_to_global(patch_indices[ti + 2]));
        }

        ++report.holes_filled;
        report.triangles_added += static_cast<crd::u32>(patch_indices.size() / 3U);
    }

    HalfEdgeMesh<T> output{alloc};
    const auto bs = output.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{global_positions.data(), global_positions.size()},
        crd::containers::ConstSpan<crd::u32>{global_indices.data(), global_indices.size()});
    (void)bs;

    if (loops.empty())
    {
        report.status = FillHolesStatus::NoHolesToFill;
    }
    report.output_vertices = output.vertex_count();
    report.output_faces    = output.face_count();
    report_out();
    return output;
}

template HalfEdgeMesh<crd::f32> fill_holes<crd::f32>(const HalfEdgeMesh<crd::f32>&,
                                                      const FillHolesOptions<crd::f32>&,
                                                      FillHolesReport*);
template HalfEdgeMesh<crd::f64> fill_holes<crd::f64>(const HalfEdgeMesh<crd::f64>&,
                                                      const FillHolesOptions<crd::f64>&,
                                                      FillHolesReport*);

} // namespace crd::geometry::mesh_processing
