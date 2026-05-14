#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — convex-hull vertex-adjacency helper (Phase
// 3.1.7 v3c-c; ADR-0076 §4 pin #14 / §15 / §18).
//
// Computes the per-vertex neighbor list for a closed convex polyhedron given
// its face-vertex topology (the `face_vertex_indices` + `face_vertex_offsets`
// arrays of `ConvexHullView<T>`). Output is the prefix-sum form expected by
// `ConvexHullView::vertex_adjacency_indices` / `vertex_adjacency_offsets`
// (the v2g hill-climb hull-support path).
//
// **Algorithm**: walk each face's vertex sequence in order. Every consecutive
// pair (including the wrap-around closing edge) is an undirected hull edge.
// Collect per-vertex neighbor sets (dedup on insert), then flatten into the
// `(indices, offsets)` prefix-sum form.
//
// **Determinism**: for each vertex, neighbors appear in the order they were
// first encountered while walking faces in face-index order. Deterministic
// across compilers / SIMD widths / OSes (no sort; insertion order is
// determined by the input face_vertex_indices order).
//
// **Output contract** (ADR-0076 §4 pin #14):
//   - EDGE-SYMMETRIC: every edge `(u, v)` appears in both `u`'s neighbor list
//     and `v`'s neighbor list.
//   - DUPLICATE-FREE: each vertex appears at most once in any neighbor list.
//   - `vertex_adjacency_offsets.size() == num_vertices + 1` (prefix-sum).
//   - `vertex_adjacency_offsets[i]` is the start of vertex `i`'s neighbor
//     slice in `vertex_adjacency_indices`; the slice extends to
//     `vertex_adjacency_offsets[i + 1]`.
//
// **Allocator**: caller supplies the backing `Array<u32>&` references; the
// arrays' existing allocators are used. Cleared before writing.
//
// **Consumers**: `crd-geometry-convex::enrich_for_gjk(QuickhullResult&)` (v3c-c),
// `tests/geometry-convex/test_hill_climb.cpp` (was the originator of this
// helper before v3c-c promotion), future V-HACD cooker, eylem
// `Collider::ConvexHull` cooker.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::geometry::primitives
{
// Compute the per-vertex neighbor list from face-vertex topology. Both
// output arrays are cleared before writing.
//
// `num_vertices` — the size of the hull's vertex array. Adjacency-output
// offset array will have size `num_vertices + 1`.
inline void compute_vertex_adjacency_from_faces(
    crd::containers::ConstSpan<crd::u32> face_vertex_indices,
    crd::containers::ConstSpan<crd::u32> face_vertex_offsets, crd::usize num_vertices,
    crd::containers::Array<crd::u32>& out_indices,
    crd::containers::Array<crd::u32>& out_offsets) noexcept
{
    out_indices.clear();
    out_offsets.clear();

    // Per-vertex neighbor lists. We use a single flat scratch `Array<u32>` per
    // vertex via the caller's existing allocator (out_indices.allocator()).
    // For determinism, neighbors are inserted in face-walk order with dedup.
    crd::containers::Array<crd::containers::Array<crd::u32>> per_vertex(out_indices.allocator());
    per_vertex.reserve(num_vertices);
    for (crd::usize i = 0; i < num_vertices; ++i)
    {
        per_vertex.emplace_back(out_indices.allocator());
    }

    auto add_neighbor = [](crd::containers::Array<crd::u32>& nbrs, crd::u32 nbr) {
        for (crd::usize i = 0; i < nbrs.size(); ++i)
        {
            if (nbrs[i] == nbr)
            {
                return;
            }
        }
        nbrs.push_back(nbr);
    };

    const crd::u32 num_faces =
        face_vertex_offsets.size() > 0 ? static_cast<crd::u32>(face_vertex_offsets.size() - 1) : 0U;
    for (crd::u32 f = 0; f < num_faces; ++f)
    {
        const crd::u32 begin = face_vertex_offsets[f];
        const crd::u32 end = face_vertex_offsets[f + 1];
        const crd::u32 n = end - begin;
        for (crd::u32 k = 0; k < n; ++k)
        {
            const crd::u32 u = face_vertex_indices[begin + k];
            const crd::u32 v = face_vertex_indices[begin + (k + 1U) % n];
            add_neighbor(per_vertex[u], v);
            add_neighbor(per_vertex[v], u);
        }
    }

    // Flatten to prefix-sum form.
    out_offsets.push_back(0);
    for (crd::usize i = 0; i < num_vertices; ++i)
    {
        for (crd::usize j = 0; j < per_vertex[i].size(); ++j)
        {
            out_indices.push_back(per_vertex[i][j]);
        }
        out_offsets.push_back(static_cast<crd::u32>(out_indices.size()));
    }
}

} // namespace crd::geometry::primitives
