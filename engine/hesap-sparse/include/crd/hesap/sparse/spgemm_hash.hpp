#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Hash-accumulator spgemm path (v1g-2) -- lifts the dense-SPA `kMaxSpaCols`
// (4M) ceiling so C = A*B works for ARBITRARY B.cols (large graphs etc.). The
// dense SPA costs O(B.cols) memory/worker; this hash costs O(row distinct nnz)
// -- bounded by the row's flop count, independent of B.cols.
//
// BIT-EXACT with the dense SPA: contributions accumulate in the IDENTICAL
// encounter order (A[i] stored order, then B[k] stored order; first touch
// writes `=prod`, repeats `+= prod`) and the row is emitted column-sorted. The
// hash only maps column -> slot; it never changes the arithmetic order, so the
// per-entry value is the same bit pattern as the dense path. Each C row is
// independent -> parallel is bit-exact with serial at any worker count.
// -----------------------------------------------------------------------

namespace detail
{
inline constexpr crd::u32 kHashEmpty = 0xFFFFFFFFU;

inline crd::u32 next_pow2(crd::u32 v) noexcept
{
    crd::u32 p = 8;
    while (p < v)
    {
        p <<= 1U;
    }
    return p;
}

// Open-addressing (linear-probe) per-row column->value accumulator. Capacity
// grows monotonically to the high-water mark; cleared per row via the used-list
// (no full wipe). Move-only (owns Arrays); one instance per parallel job.
template <typename T>
struct SpgemmHash
{
    crd::containers::Array<crd::u32> slot_key;  // capacity; kHashEmpty == free
    crd::containers::Array<T>        slot_val;  // capacity
    crd::containers::Array<crd::u32> used;      // occupied slot indices this row
    crd::containers::Array<crd::u32> order;     // emit scratch (used-slots sorted by column)
    crd::u32                         capacity = 0;
    crd::u32                         mask     = 0;

    explicit SpgemmHash(crd::memory::IAllocator* alloc)
        : slot_key(alloc), slot_val(alloc), used(alloc), order(alloc)
    {
    }

    SpgemmHash(SpgemmHash&&) noexcept            = default;
    SpgemmHash& operator=(SpgemmHash&&) noexcept = default;
    SpgemmHash(const SpgemmHash&)                = delete;
    SpgemmHash& operator=(const SpgemmHash&)     = delete;

    // Pre-allocate ALL scratch (slots + used + order) for `distinct_ub` distinct
    // columns -- call once, single-threaded, BEFORE parallel use. Then per-row
    // reserve_row/touch/emit never allocate (the TlsfAllocator is not
    // thread-safe; allocating inside a parallel_for corrupts the heap).
    void preinit(crd::u32 distinct_ub)
    {
        const crd::u32 need = next_pow2(distinct_ub == 0 ? 1U : (distinct_ub * 2U));
        slot_key.resize(need);
        slot_val.resize(need);
        for (crd::u32 s = 0; s < need; ++s)
        {
            slot_key[s] = kHashEmpty;
        }
        used.reserve(need);
        order.reserve(need);
        capacity = need;
        mask     = need - 1U;
    }

    // Ensure room for `distinct_ub` distinct columns at <=0.5 load. All slots
    // are kHashEmpty on entry (begin-of-row invariant), so growth only inits the
    // newly added slots.
    void reserve_row(crd::u32 distinct_ub)
    {
        const crd::u32 need = next_pow2(distinct_ub == 0 ? 1U : (distinct_ub * 2U));
        if (need > capacity)
        {
            const crd::u32 old = capacity;
            slot_key.resize(need);
            slot_val.resize(need);
            for (crd::u32 s = old; s < need; ++s)
            {
                slot_key[s] = kHashEmpty;
            }
            capacity = need;
            mask     = need - 1U;
        }
    }

    // Find/insert `key`; sets is_new. Caller owns the value write (so first-touch
    // `=prod` vs repeat `+=prod` matches the dense SPA exactly).
    [[nodiscard]] crd::u32 touch(crd::u32 key, bool& is_new) noexcept
    {
        crd::u32 h = (key * 2654435761U) & mask;
        for (;;)
        {
            const crd::u32 cur = slot_key[h];
            if (cur == kHashEmpty)
            {
                slot_key[h] = key;
                used.push_back(h);
                is_new = true;
                return h;
            }
            if (cur == key)
            {
                is_new = false;
                return h;
            }
            h = (h + 1U) & mask;
        }
    }

    void clear_row() noexcept
    {
        for (crd::usize t = 0; t < used.size(); ++t)
        {
            slot_key[used[t]] = kHashEmpty;
        }
        used.clear();
    }
};

// Flop upper bound for row i = sum_{k in A[i]} nnz(B[k]) (>= distinct columns).
template <typename T>
crd::u32 spgemm_row_flop_ub(const SparseMatrix<T, SparseFormat::Csr>& a, const SparseMatrix<T, SparseFormat::Csr>& b,
                            crd::u32 i) noexcept
{
    const crd::u32* ao = a.pattern().outer_ptr.data();
    const crd::u32* ai = a.pattern().inner_idx.data();
    const crd::u32* bo = b.pattern().outer_ptr.data();
    crd::u32        f  = 0;
    for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
    {
        f += bo[ai[ka] + 1] - bo[ai[ka]];
    }
    return f;
}

// Distinct-column COUNT for row i (symbolic phase). Hash cleared on exit.
template <typename T>
crd::u32 spgemm_row_hash_count(const SparseMatrix<T, SparseFormat::Csr>& a,
                               const SparseMatrix<T, SparseFormat::Csr>& b, crd::u32 i, SpgemmHash<T>& h)
{
    const crd::u32* ao = a.pattern().outer_ptr.data();
    const crd::u32* ai = a.pattern().inner_idx.data();
    const crd::u32* bo = b.pattern().outer_ptr.data();
    const crd::u32* bi = b.pattern().inner_idx.data();
    h.reserve_row(spgemm_row_flop_ub<T>(a, b, i));
    for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
    {
        const crd::u32 k = ai[ka];
        for (crd::u32 kb = bo[k]; kb < bo[k + 1]; ++kb)
        {
            bool isnew = false;
            (void)h.touch(bi[kb], isnew);
        }
    }
    const crd::u32 cnt = static_cast<crd::u32>(h.used.size());
    h.clear_row();
    return cnt;
}

// Numeric accumulate of row i into [w0, w1) of out_inner/out_vals (sorted).
// Hash cleared on exit. `order` is reused scratch (sized inside).
template <typename T>
void spgemm_row_hash_numeric(const SparseMatrix<T, SparseFormat::Csr>& a, const SparseMatrix<T, SparseFormat::Csr>& b,
                             crd::u32 i, SpgemmHash<T>& h, crd::u32* out_inner, T* out_vals)
{
    crd::containers::Array<crd::u32>& order = h.order;
    const crd::u32* ao = a.pattern().outer_ptr.data();
    const crd::u32* ai = a.pattern().inner_idx.data();
    const T*        av = a.values().values.data();
    const crd::u32* bo = b.pattern().outer_ptr.data();
    const crd::u32* bi = b.pattern().inner_idx.data();
    const T*        bv = b.values().values.data();
    h.reserve_row(spgemm_row_flop_ub<T>(a, b, i));
    for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
    {
        const crd::u32 k    = ai[ka];
        const T        aval = av[ka];
        for (crd::u32 kb = bo[k]; kb < bo[k + 1]; ++kb)
        {
            const T  prod = aval * bv[kb];
            bool     isnew = false;
            const crd::u32 slot = h.touch(bi[kb], isnew);
            h.slot_val[slot]    = isnew ? prod : (h.slot_val[slot] + prod);  // same order as dense SPA
        }
    }
    // Emit column-sorted: sort the used slots by their column key.
    order.clear();
    for (crd::usize t = 0; t < h.used.size(); ++t)
    {
        order.push_back(h.used[t]);
    }
    const crd::u32* keys = h.slot_key.data();
    crd::containers::sort(order.data(), order.data() + order.size(),
                          [keys](crd::u32 sa, crd::u32 sb) { return keys[sa] < keys[sb]; });
    for (crd::usize t = 0; t < order.size(); ++t)
    {
        out_inner[t] = h.slot_key[order[t]];
        out_vals[t]  = h.slot_val[order[t]];
    }
    h.clear_row();
}

// Serial fused hash spgemm over rows [i_begin, i_end) (mirrors spgemm_rows).
template <typename T>
void spgemm_rows_hash(const SparseMatrix<T, SparseFormat::Csr>& a, const SparseMatrix<T, SparseFormat::Csr>& b,
                      crd::u32 i_begin, crd::u32 i_end, SpgemmHash<T>& h,
                      crd::containers::Array<crd::u32>& out_inner, crd::containers::Array<T>& out_vals,
                      crd::containers::Array<crd::u32>& out_outer)
{
    crd::containers::Array<crd::u32>& order = h.order;
    for (crd::u32 i = i_begin; i < i_end; ++i)
    {
        h.reserve_row(spgemm_row_flop_ub<T>(a, b, i));
        const crd::u32* ao = a.pattern().outer_ptr.data();
        const crd::u32* ai = a.pattern().inner_idx.data();
        const T*        av = a.values().values.data();
        const crd::u32* bo = b.pattern().outer_ptr.data();
        const crd::u32* bi = b.pattern().inner_idx.data();
        const T*        bv = b.values().values.data();
        for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
        {
            const crd::u32 k    = ai[ka];
            const T        aval = av[ka];
            for (crd::u32 kb = bo[k]; kb < bo[k + 1]; ++kb)
            {
                const T        prod = aval * bv[kb];
                bool           isnew = false;
                const crd::u32 slot  = h.touch(bi[kb], isnew);
                h.slot_val[slot]     = isnew ? prod : (h.slot_val[slot] + prod);
            }
        }
        order.clear();
        for (crd::usize t = 0; t < h.used.size(); ++t)
        {
            order.push_back(h.used[t]);
        }
        const crd::u32* keys = h.slot_key.data();
        crd::containers::sort(order.data(), order.data() + order.size(),
                              [keys](crd::u32 sa, crd::u32 sb) { return keys[sa] < keys[sb]; });
        for (crd::usize t = 0; t < order.size(); ++t)
        {
            out_inner.push_back(h.slot_key[order[t]]);
            out_vals.push_back(h.slot_val[order[t]]);
        }
        out_outer[i + 1] = static_cast<crd::u32>(out_inner.size());
        h.clear_row();
    }
}
} // namespace detail
} // namespace crd::hesap::sparse
