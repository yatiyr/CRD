#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// AnalysisHandle — the cache leg of the trinity ("the heart"). When a
// backend analyses a SparsePattern (ordering, symbolic factorisation, exec
// format selection, scratch layout) it stamps the result with the pattern's
// `topology_hash`. The plan stays valid as long as the structure is
// unchanged — values may churn frame-to-frame without re-analysis.
//
// v1a-1 ships the identity + validity contract. The `native` plan pointer
// (cuSPARSE / oneMKL / custom symbolic-factor scratch) is reserved for the
// v4/v5 consumers that actually populate it; v1a keeps it null.
//
// Validity rule (pinned): a handle is valid for a pattern iff its cached
// `topology_hash` equals the pattern's CURRENT `topology_hash`. The pattern
// owner is responsible for calling `recompute_topology_hash()` after any
// structural mutation; `is_valid_for` then reports stale handles.
// -----------------------------------------------------------------------

struct AnalysisHandle
{
    crd::u64     topology_hash = 0;
    SparseFormat recommended_format = SparseFormat::Csr;
    void*        native = nullptr;  // backend symbolic plan; null until a v4/v5 consumer fills it

    [[nodiscard]] bool is_valid_for(const SparsePattern& pattern) const noexcept
    {
        return topology_hash != 0 && topology_hash == pattern.topology_hash;
    }
};

} // namespace crd::hesap::sparse
