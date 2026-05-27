#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/convert.hpp> // transpose
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::amg
{
// ---------------------------------------------------------------------------
// rs_cf_split -- classical Ruge-Stüben coarse/fine (C/F) splitting. v4k-d.
//
// Input: the DIRECTED strength graph S (S row i = points i strongly depends on),
// from rs_strength_matrix. Output: cf[i] ∈ {kFine, kCoarse} for every point, and
// the number of C-points (n_coarse).
//
// Algorithm (Ruge-Stüben 1987, first / "standard" pass):
//   measure λ_i = |{ j : j strongly depends on i }| = in-degree in S = |Sᵀ row i|.
//   while ∃ undecided point with λ > 0:
//     pick the undecided point i with MAXIMUM λ (ascending-index tie-break) → C.
//     every undecided j that strongly depends on i (j ∈ Sᵀ[i]) → F, and each of
//       that F-point's undecided strong dependencies k ∈ S[j] gets λ_k += 1
//       (it becomes a more valuable future C-point).
//     every undecided j that i strongly depends on (j ∈ S[i]) gets λ_j -= 1.
//   any remaining undecided point (λ collapsed to 0, e.g. isolated) → F.
//
// This guarantees every F-point strongly depends on ≥1 C-point (it was turned F
// because it depended on the just-selected C), so direct interpolation is always
// well-defined. The optional second pass (F-F pairs share a common C) is a
// later interpolation-quality refinement, not required for correctness.
//
// Deterministic: max-λ ties break to the LOWEST index (bucketed by λ, scanned
// ascending within a bucket). No RNG. (D(amg) coarsening tie-break.)
// ---------------------------------------------------------------------------

enum class CfTag : crd::u8 { Fine = 0, Coarse = 1 };

template <typename T>
[[nodiscard]] crd::containers::Array<CfTag>
rs_cf_split(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& s, crd::u32& n_coarse_out,
            crd::memory::IAllocator* alloc)
{
    const crd::u32 n = s.rows();
    const auto*    so = s.pattern().outer_ptr.data(); // S row i = i's strong dependencies
    const auto*    si = s.pattern().inner_idx.data();
    // Sᵀ: row i = points that strongly depend on i (i's "influence" set).
    auto        st  = crd::hesap::sparse::transpose<T>(s, alloc);
    const auto* sto = st.pattern().outer_ptr.data();
    const auto* sti = st.pattern().inner_idx.data();

    constexpr crd::u32 kUndecided = 0, kFmark = 1, kCmark = 2;
    crd::containers::Array<crd::u8>  state(alloc);
    crd::containers::Array<crd::i32> lambda(alloc); // signed (decrements can go below initial)
    state.resize(n == 0 ? 1 : n);
    lambda.resize(n == 0 ? 1 : n);
    crd::i32 maxlam = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        state[i]  = kUndecided;
        lambda[i] = static_cast<crd::i32>(sto[i + 1] - sto[i]); // |Sᵀ row i|
        if (lambda[i] > maxlam) { maxlam = lambda[i]; }
    }

    // Bucketed max-priority: bucket[v] = doubly-linked list of undecided points with λ==v.
    // O(1) move on λ change, O(maxλ) to find the current max. next/prev are intrusive.
    // λ GROWS past its initial max via the +1 bumps (new F-points raise their deps' λ); it is
    // bounded by n (it counts points), so size the buckets to n+2 to hold any reachable λ.
    const crd::u32 nb = n + 2;
    (void)maxlam;
    crd::containers::Array<crd::i32> head(alloc), nxt(alloc), prv(alloc);
    head.resize(nb); nxt.resize(n == 0 ? 1 : n); prv.resize(n == 0 ? 1 : n);
    for (crd::u32 v = 0; v < nb; ++v) { head[v] = -1; }
    auto bucket_push = [&](crd::u32 i) {
        const crd::i32 v = lambda[i] < 0 ? 0 : lambda[i];
        nxt[i] = head[v]; prv[i] = -1;
        if (head[v] >= 0) { prv[head[v]] = static_cast<crd::i32>(i); }
        head[v] = static_cast<crd::i32>(i);
    };
    auto bucket_remove = [&](crd::u32 i) {
        const crd::i32 v = lambda[i] < 0 ? 0 : lambda[i];
        if (prv[i] >= 0) { nxt[prv[i]] = nxt[i]; } else { head[v] = nxt[i]; }
        if (nxt[i] >= 0) { prv[nxt[i]] = prv[i]; }
    };
    for (crd::u32 i = 0; i < n; ++i) { if (state[i] == kUndecided && lambda[i] > 0) { bucket_push(i); } }

    crd::u32 cur_max = (maxlam > 0) ? static_cast<crd::u32>(maxlam) : 0;
    auto bump = [&](crd::u32 k, crd::i32 delta) {
        if (state[k] != kUndecided || lambda[k] <= 0) { lambda[k] += delta; return; } // not in a bucket
        bucket_remove(k);
        lambda[k] += delta;
        if (lambda[k] > 0)
        {
            bucket_push(k);
            if (static_cast<crd::u32>(lambda[k]) > cur_max) { cur_max = static_cast<crd::u32>(lambda[k]); }
        }
    };

    crd::u32 n_coarse = 0;
    for (;;)
    {
        while (cur_max > 0 && head[cur_max] < 0) { --cur_max; }
        if (cur_max == 0) { break; } // no undecided point with λ>0 left
        // lowest-index point in the top bucket (deterministic tie-break).
        crd::i32 pick = -1;
        for (crd::i32 c = head[cur_max]; c >= 0; c = nxt[c]) { if (pick < 0 || c < pick) { pick = c; } }
        const crd::u32 i = static_cast<crd::u32>(pick);
        bucket_remove(i);
        state[i] = kCmark;
        ++n_coarse;
        // every undecided j that depends on i → F; bump each F's strong deps.
        for (crd::u32 q = sto[i]; q < sto[i + 1]; ++q)
        {
            const crd::u32 j = sti[q];
            if (state[j] != kUndecided) { continue; }
            bucket_remove(j);
            state[j] = kFmark;
            for (crd::u32 r = so[j]; r < so[j + 1]; ++r)
            {
                const crd::u32 k = si[r];
                if (state[k] == kUndecided) { bump(k, +1); }
            }
        }
        // points i depends on become slightly less valuable.
        for (crd::u32 q = so[i]; q < so[i + 1]; ++q)
        {
            const crd::u32 j = si[q];
            if (state[j] == kUndecided) { bump(j, -1); }
        }
    }

    // Interpolation-correctness pass: a fine point with strong dependencies must have at
    // least one COARSE strong dependency (else direct interpolation has an empty coarse set).
    // First-pass leftovers (λ collapsed to 0 ⇒ marked F) can violate this. Promote any such
    // F-point to C and iterate to fixpoint (monotone — only adds C ⇒ terminates). This is a
    // simpler, always-correct alternative to the classical "second pass" (which optimizes WHICH
    // point to promote; here we just guarantee the property). Deterministic: ascending scan.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (state[i] == kCmark || so[i] == so[i + 1]) { continue; } // C, or no strong deps
            bool has_c = false;
            for (crd::u32 q = so[i]; q < so[i + 1]; ++q) { if (state[si[q]] == kCmark) { has_c = true; break; } }
            if (!has_c) { state[i] = kCmark; ++n_coarse; changed = true; }
        }
    }

    crd::containers::Array<CfTag> cf(alloc);
    cf.resize(n == 0 ? 1 : n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        cf[i] = (state[i] == kCmark) ? CfTag::Coarse : CfTag::Fine; // undecided/F ⇒ Fine
    }
    n_coarse_out = n_coarse;
    return cf;
}

} // namespace crd::hesap::amg
