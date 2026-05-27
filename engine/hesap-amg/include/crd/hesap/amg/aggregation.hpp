#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::amg
{
// ---------------------------------------------------------------------------
// aggregate -- greedy aggregation (Vaněk-Mandel-Brezina 1996, Alg. 4.1).
// Phase 3.1.6 v4k-a.
//
// Given the symmetrized strong-connection graph S (from strength_matrix),
// partition the nodes into disjoint AGGREGATES (the coarse-grid points):
//   Pass 1 — seed: scan nodes in ascending index; a node whose ENTIRE strong
//            neighbourhood is still free starts a new aggregate = {i} ∪ N_S(i).
//   Pass 2 — extend: an unaggregated node adjacent to an existing aggregate
//            joins it (lowest aggregate id on ties, D(amg)-2).
//   Pass 3 — leftover: remaining unaggregated nodes seed new aggregates from
//            whatever free strong neighbours remain (or a singleton).
//
// Returns agg[i] = aggregate id of node i (0..n_agg-1); n_agg via out param.
// Deterministic (D(amg)-1/2): ascending node + column order, lowest-id tie-break.
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] crd::containers::Array<crd::u32>
aggregate(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& s,
          crd::u32& n_agg_out, crd::memory::IAllocator* alloc)
{
    const crd::u32 n     = s.rows();
    const auto*    outer = s.pattern().outer_ptr.data();
    const auto*    inner = s.pattern().inner_idx.data();

    constexpr crd::u32 kFree = ~crd::u32{0};
    crd::containers::Array<crd::u32> agg(alloc);
    agg.resize(n);
    for (crd::u32 i = 0; i < n; ++i) { agg[i] = kFree; }
    crd::u32 n_agg = 0;

    // Pass 1 — seed aggregates from fully-free strong neighbourhoods.
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (agg[i] != kFree) { continue; }
        bool all_free = true;
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { if (agg[inner[q]] != kFree) { all_free = false; break; } }
        if (!all_free) { continue; }
        const crd::u32 id = n_agg++;
        agg[i]            = id;
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { agg[inner[q]] = id; }
    }

    // Pass 2 — extend: join an unaggregated node to an adjacent aggregate (lowest id).
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (agg[i] != kFree) { continue; }
        crd::u32 best = kFree;
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
        {
            const crd::u32 g = agg[inner[q]];
            if (g != kFree && g < best) { best = g; }
        }
        if (best != kFree) { agg[i] = best; }
    }

    // Pass 3 — leftover unaggregated nodes seed new aggregates from free neighbours.
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (agg[i] != kFree) { continue; }
        const crd::u32 id = n_agg++;
        agg[i]            = id;
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { if (agg[inner[q]] == kFree) { agg[inner[q]] = id; } }
    }

    n_agg_out = n_agg;
    return agg;
}

} // namespace crd::hesap::amg
