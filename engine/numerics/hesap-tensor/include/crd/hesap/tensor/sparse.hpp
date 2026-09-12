#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-i: SPARSE tensors. COO (canonical sorted+deduped
// coordinate format, SoA index planes) + CSF (compressed sparse fiber — the
// SPLATT mode tree) + sparse x dense mode-n contraction (TTM) + sparse
// elementwise (add/mul vs sparse and dense) + reductions (sum/max, total and
// per mode, DENSE semantics: implicit zeros participate in max exactly like
// a densified array). MTTKRP (the CP-decomposition workhorse) lives in
// sparse_mttkrp.hpp.
//
// Reuse per SANITY #8: the builder mirrors hesap-sparse's TripletBuilder
// determinism contract (stable counting-sort grouping, insertion-order
// duplicate summation — bit-reproducible for a fixed triplet sequence),
// generalized from (row,col) to rank-N lexicographic LSD counting sort.
// hesap-sparse's 2-D CSR machinery itself is matrix-shaped (SparsePattern is
// rows/cols); the N-D formats live here in the tensor module (their home).
//
// Determinism: every operation enumerates nonzeros in the canonical sorted
// order; parallel drivers partition by a grain that is a function of
// shape/nnz ONLY with disjoint outputs (or a fixed-count partial-buffer
// reduction folded in task order) ⇒ results are BIT-IDENTICAL at any worker
// count (the {1..16} moat, gated).
//
// Error contract: TensorStatus returns, never throws; programmer errors are
// CRD_ASSERT in debug (the v13 pillars). Indices are u32 (dims < 2^32,
// nnz < 2^32 — validated). All storage via IAllocator*, never
// default_allocator.
// ---------------------------------------------------------------------------
#include "reduce.hpp" // detail::block_partial — the module's pinned-order reduction core
#include "tensor.hpp"

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/simd.hpp>

#include <cmath>   // std::fma — the single-rounded scalar chain (codelet rule)
#include <cstring> // memset/memcpy on trivially-copyable planes

namespace crd::hesap::tensor
{

// -----------------------------------------------------------------------
// SparseCoo<T> — canonical coordinate tensor: indices sorted lexicographically
// (mode 0 major), duplicates summed. Storage is SoA: one u32 plane per mode,
// concatenated mode-major in a single allocation (idx(m) = plane pointer).
// Filled by SparseCooBuilder::compress or the sparse ops; init() is the
// (public) storage hook those writers use.
// -----------------------------------------------------------------------
template <typename T> class SparseCoo
{
    static_assert(std::is_floating_point_v<T>, "SparseCoo<T>: T must be f32/f64");

public:
    explicit SparseCoo(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_idx(alloc), m_val(alloc)
    {
        CRD_ASSERT_MSG(alloc != nullptr, "SparseCoo: null allocator");
    }

    // (Re)shape + size the storage; index planes / values are uninitialized
    // (the writer fills them). Validates the u32-index contract.
    [[nodiscard]] TensorStatus init(crd::containers::ConstSpan<crd::u64> shape, crd::u64 nnz)
    {
        if (shape.size() == 0U || shape.size() > kMaxRank || nnz > 0xFFFFFFFFULL)
        {
            return TensorStatus::BadInput;
        }
        for (crd::u64 s : shape)
        {
            if (s > 0xFFFFFFFFULL)
            {
                return TensorStatus::BadInput;
            }
        }
        m_rank = static_cast<crd::u32>(shape.size());
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            m_shape[m] = shape[m];
        }
        m_nnz = nnz;
        m_idx.resize_uninitialized(static_cast<crd::usize>(m_rank) * static_cast<crd::usize>(nnz));
        m_val.resize_uninitialized(static_cast<crd::usize>(nnz));
        return TensorStatus::Ok;
    }

    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }
    [[nodiscard]] crd::u32 rank() const noexcept { return m_rank; }
    [[nodiscard]] crd::u64 nnz() const noexcept { return m_nnz; }
    [[nodiscard]] crd::u64 shape(crd::u32 mode) const noexcept
    {
        CRD_ASSERT_MSG(mode < m_rank, "SparseCoo::shape: mode out of range");
        return m_shape[mode];
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u64> shape() const noexcept { return {m_shape, m_rank}; }

    // logical element count (product of dims)
    [[nodiscard]] crd::u64 size() const noexcept
    {
        crd::u64 n = 1;
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            n *= m_shape[m];
        }
        return n;
    }

    [[nodiscard]] const crd::u32* idx(crd::u32 mode) const noexcept
    {
        CRD_ASSERT_MSG(mode < m_rank, "SparseCoo::idx: mode out of range");
        return m_idx.data() + static_cast<crd::usize>(mode) * static_cast<crd::usize>(m_nnz);
    }
    [[nodiscard]] crd::u32* idx_mut(crd::u32 mode) noexcept
    {
        CRD_ASSERT_MSG(mode < m_rank, "SparseCoo::idx_mut: mode out of range");
        return m_idx.data() + static_cast<crd::usize>(mode) * static_cast<crd::usize>(m_nnz);
    }
    [[nodiscard]] const T* val() const noexcept { return m_val.data(); }
    [[nodiscard]] T* val_mut() noexcept { return m_val.data(); }

    // Shrink to `new_nnz` entries after a worst-case-sized merge fill: packs
    // the mode planes from the old stride to the new one (ascending mode
    // order — the destination never overruns the source) and trims storage.
    void compact_to(crd::u64 new_nnz) noexcept
    {
        CRD_ASSERT_MSG(new_nnz <= m_nnz, "SparseCoo::compact_to: cannot grow");
        if (new_nnz == m_nnz)
        {
            return;
        }
        for (crd::u32 m = 1; m < m_rank; ++m)
        {
            std::memmove(m_idx.data() + static_cast<crd::usize>(m) * static_cast<crd::usize>(new_nnz),
                         m_idx.data() + static_cast<crd::usize>(m) * static_cast<crd::usize>(m_nnz),
                         static_cast<crd::usize>(new_nnz) * sizeof(crd::u32));
        }
        m_nnz = new_nnz;
        m_idx.resize(static_cast<crd::usize>(m_rank) * static_cast<crd::usize>(new_nnz));
        m_val.resize(static_cast<crd::usize>(new_nnz));
    }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_rank = 0;
    crd::u64 m_shape[kMaxRank] = {};
    crd::u64 m_nnz = 0;
    crd::containers::Array<crd::u32> m_idx; // mode-major planes [rank][nnz]
    crd::containers::Array<T> m_val;
};

namespace sparsedetail
{

// lexicographic tuple compare over the SoA planes: -1 / 0 / +1
template <typename T>
[[nodiscard]] inline crd::i32 cmp_tuple(const SparseCoo<T>& a, crd::u64 i, const SparseCoo<T>& b,
                                        crd::u64 j) noexcept
{
    for (crd::u32 m = 0; m < a.rank(); ++m)
    {
        const crd::u32 x = a.idx(m)[i];
        const crd::u32 y = b.idx(m)[j];
        if (x != y)
        {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

// Stable LSD counting sort of `n` items by rank-many u32 keys. key(item, level)
// returns the level's key; levels are sorted last-to-first so the final order
// is lexicographic in level order. perm_a holds the identity on entry and the
// final permutation on exit; perm_b and counts are caller scratch (counts must
// hold max_key+2 entries). Deterministic and stable (insertion order preserved
// within equal keys) — the TripletBuilder contract, rank-N.
template <typename KeyFn>
inline void lsd_sort(crd::u32* perm_a, crd::u32* perm_b, crd::u32* counts, crd::u64 n, crd::u32 levels,
                     const crd::u64* level_dims, KeyFn&& key) noexcept
{
    for (crd::u32 pass = 0; pass < levels; ++pass)
    {
        const crd::u32 l = levels - 1U - pass;
        const crd::u64 dim = level_dims[l];
        std::memset(counts, 0, static_cast<crd::usize>(dim + 1U) * sizeof(crd::u32));
        for (crd::u64 i = 0; i < n; ++i)
        {
            ++counts[key(perm_a[i], l) + 1U];
        }
        for (crd::u64 d = 0; d < dim; ++d)
        {
            counts[d + 1U] += counts[d];
        }
        for (crd::u64 i = 0; i < n; ++i)
        {
            const crd::u32 item = perm_a[i];
            perm_b[counts[key(item, l)]++] = item;
        }
        crd::u32* tmp = perm_a;
        perm_a = perm_b;
        perm_b = tmp;
    }
    // levels passes: if odd, the result sits in what the caller passed as
    // perm_b — copy back so the contract is "result in perm_a".
    if ((levels & 1U) != 0U)
    {
        // after an odd number of swaps perm_a (local) is the caller's perm_b
        std::memcpy(perm_b, perm_a, static_cast<crd::usize>(n) * sizeof(crd::u32));
    }
}

// deterministic task grain: a function of shape/nnz ONLY (Tier-D rule)
[[nodiscard]] inline crd::u32 task_count(crd::u64 work_items, crd::u64 flops_per_item, crd::u32 cap) noexcept
{
    const crd::u64 per = flops_per_item > 0U ? flops_per_item : 1U;
    crd::u64 grain = (64ULL * 1024ULL + per - 1ULL) / per;
    if (grain < 1U)
    {
        grain = 1U;
    }
    crd::u64 tasks = (work_items + grain - 1ULL) / grain;
    if (tasks < 1U)
    {
        tasks = 1U;
    }
    if (tasks > cap)
    {
        tasks = cap;
    }
    return static_cast<crd::u32>(tasks);
}

// dst[q] += a * row[q] over q in [0, r) — vector fma lanes + masked partial
// tail (each element q is ONE fixed-order fma chain; lanes are lane-wise
// IEEE ⇒ identical bits on every path that uses this kernel).
template <typename T> inline void axpy_row(T* dst, const T* row, T a, crd::u64 r) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    const V av(a);
    crd::u64 q = 0;
    for (; q + W <= r; q += W)
    {
        simd::fma(av, V::load(row + q), V::load(dst + q)).store(dst + q);
    }
    if (q < r)
    {
        const crd::usize c = static_cast<crd::usize>(r - q);
        simd::fma(av, V::load_partial(row + q, c), V::load_partial(dst + q, c)).store_partial(dst + q, c);
    }
}

// dst[q] += a[q] * b[q] over q in [0, r) — same bit contract as axpy_row.
template <typename T> inline void hadamard_acc(T* dst, const T* a, const T* b, crd::u64 r) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    crd::u64 q = 0;
    for (; q + W <= r; q += W)
    {
        simd::fma(V::load(a + q), V::load(b + q), V::load(dst + q)).store(dst + q);
    }
    if (q < r)
    {
        const crd::usize c = static_cast<crd::usize>(r - q);
        simd::fma(V::load_partial(a + q, c), V::load_partial(b + q, c), V::load_partial(dst + q, c))
            .store_partial(dst + q, c);
    }
}

// dst[q] += src[q] (the partial-buffer fold)
template <typename T> inline void add_acc(T* dst, const T* src, crd::u64 r) noexcept
{
    namespace simd = crd::math::simd;
    using V = std::conditional_t<std::is_same_v<T, crd::f32>, simd::Vec8f, simd::Vec4d>;
    constexpr crd::u64 W = std::is_same_v<T, crd::f32> ? 8U : 4U;
    crd::u64 q = 0;
    for (; q + W <= r; q += W)
    {
        (V::load(dst + q) + V::load(src + q)).store(dst + q);
    }
    if (q < r)
    {
        const crd::usize c = static_cast<crd::usize>(r - q);
        (V::load_partial(dst + q, c) + V::load_partial(src + q, c)).store_partial(dst + q, c);
    }
}

// bit width needed for the largest index of a dim (dim-1); dims 0/1 need 0
[[nodiscard]] inline crd::u32 dim_bits(crd::u64 dim) noexcept
{
    if (dim <= 1U)
    {
        return 0U;
    }
    crd::u32 b = 0;
    crd::u64 v = dim - 1U;
    while (v != 0U)
    {
        ++b;
        v >>= 1U;
    }
    return b;
}

// bit layout of the packed lexicographic u64 key (mode 0 at the top bits);
// returns the total bit count (> 64 = does not fit, packing invalid)
inline crd::u32 key_layout(crd::containers::ConstSpan<crd::u64> shape, crd::u32* shift,
                           crd::u64* mask) noexcept
{
    const crd::u32 rank = static_cast<crd::u32>(shape.size());
    crd::u32 total = 0;
    for (crd::u32 m = rank; m-- > 0U;)
    {
        shift[m] = total;
        const crd::u32 b = dim_bits(shape[m]);
        mask[m] = b == 0U ? 0ULL : (b >= 64U ? ~0ULL : (1ULL << b) - 1ULL);
        total += b;
    }
    return total;
}

// Pack every entry's index tuple into ONE integer key (u32 when the tuple
// fits 32 bits, u64 up to 64) whose integer order IS the lexicographic tuple
// order (mode 0 at the top bits). Entry-major single pass: each key is
// WRITTEN once (never a read-modify-write sweep over the key array — that
// costs 3x the memory traffic at rank 3).
template <typename T, typename KT> inline void pack_keys(const SparseCoo<T>& x, KT* keys) noexcept
{
    const crd::u32 rank = x.rank();
    crd::u32 shift[kMaxRank];
    crd::u64 mask[kMaxRank];
    const crd::u32 total = key_layout(x.shape(), shift, mask);
    CRD_ASSERT_MSG(total <= 8U * sizeof(KT), "pack_keys: key does not fit the key type");
    (void)total;
    const crd::u64 n = x.nnz();
    const crd::u32* planes[kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        planes[m] = x.idx(m);
    }
    if (rank == 3U) // the dominant tensor rank — keep the inner loop flat
    {
        const crd::u32* p0 = planes[0];
        const crd::u32* p1 = planes[1];
        const crd::u32* p2 = planes[2];
        const crd::u32 s0 = shift[0];
        const crd::u32 s1 = shift[1];
        for (crd::u64 e = 0; e < n; ++e)
        {
            keys[e] = static_cast<KT>((static_cast<crd::u64>(p0[e]) << s0) |
                                      (static_cast<crd::u64>(p1[e]) << s1) | static_cast<crd::u64>(p2[e]));
        }
        return;
    }
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u64 k = 0;
        for (crd::u32 m = 0; m < rank; ++m)
        {
            k |= static_cast<crd::u64>(planes[m][e]) << shift[m];
        }
        keys[e] = static_cast<KT>(k);
    }
}

// FORCED-branchless union merge of two strictly-increasing key sequences
// (the merge decisions are data-random — a predictor-hostile pattern; the
// naive ternary/branchy forms measured 1.5x slower). Value selection rides
// integer bit masks (an absent side contributes an explicit +0.0 — exactly
// the densified reference's arithmetic, bit-gated). RankC == 3 gets a flat
// unpack; RankC == 0 is the generic rank loop.
template <typename T, typename KT, crd::u32 RankC>
inline crd::u64 merge_union(const KT* ka, crd::u64 na, const T* av, const KT* kb, crd::u64 nb, const T* bv,
                            crd::u32 rank, const crd::u32* shift, const crd::u64* mask,
                            crd::u32* const* op, T* ov, crd::u64& end_i, crd::u64& end_j) noexcept
{
    using TB = std::conditional_t<std::is_same_v<T, crd::f32>, crd::u32, crd::u64>;
    crd::u64 i = 0;
    crd::u64 j = 0;
    crd::u64 w = 0;
    while (i < na && j < nb)
    {
        const KT ki = ka[i];
        const KT kj = kb[j];
        const bool ta = ki <= kj;
        const bool tb = kj <= ki;
        const KT kmask = static_cast<KT>(KT(0) - static_cast<KT>(ta));
        const crd::u64 key = (static_cast<crd::u64>(ki) & static_cast<crd::u64>(kmask)) |
                             (static_cast<crd::u64>(kj) & ~static_cast<crd::u64>(kmask));
        const TB ma = static_cast<TB>(TB(0) - static_cast<TB>(ta));
        const TB mb = static_cast<TB>(TB(0) - static_cast<TB>(tb));
        const T va2 = std::bit_cast<T>(static_cast<TB>(std::bit_cast<TB>(av[i]) & ma));
        const T vb2 = std::bit_cast<T>(static_cast<TB>(std::bit_cast<TB>(bv[j]) & mb));
        ov[w] = va2 + vb2;
        if constexpr (RankC == 3U)
        {
            op[0][w] = static_cast<crd::u32>(key >> shift[0]);
            op[1][w] = static_cast<crd::u32>((key >> shift[1]) & mask[1]);
            op[2][w] = static_cast<crd::u32>(key & mask[2]);
        }
        else
        {
            for (crd::u32 m = 0; m < rank; ++m)
            {
                op[m][w] = static_cast<crd::u32>((key >> shift[m]) & mask[m]);
            }
        }
        ++w;
        i += static_cast<crd::u64>(ta);
        j += static_cast<crd::u64>(tb);
    }
    end_i = i;
    end_j = j;
    return w;
}

// forced-branchless intersection merge: unconditional store at w, w advances
// only on a key match (the slot is overwritten otherwise; bounded by
// min(na, nb) — w == cap implies a cursor is exhausted)
template <typename T, typename KT, crd::u32 RankC>
inline crd::u64 merge_intersect(const KT* ka, crd::u64 na, const T* av, const KT* kb, crd::u64 nb,
                                const T* bv, crd::u32 rank, const crd::u32* shift, const crd::u64* mask,
                                crd::u32* const* op, T* ov) noexcept
{
    crd::u64 i = 0;
    crd::u64 j = 0;
    crd::u64 w = 0;
    while (i < na && j < nb)
    {
        const KT ki = ka[i];
        const KT kj = kb[j];
        const crd::u64 key = static_cast<crd::u64>(ki);
        if constexpr (RankC == 3U)
        {
            op[0][w] = static_cast<crd::u32>(key >> shift[0]);
            op[1][w] = static_cast<crd::u32>((key >> shift[1]) & mask[1]);
            op[2][w] = static_cast<crd::u32>(key & mask[2]);
        }
        else
        {
            for (crd::u32 m = 0; m < rank; ++m)
            {
                op[m][w] = static_cast<crd::u32>((key >> shift[m]) & mask[m]);
            }
        }
        ov[w] = av[i] * bv[j];
        w += static_cast<crd::u64>(ki == kj);
        i += static_cast<crd::u64>(ki <= kj);
        j += static_cast<crd::u64>(kj <= ki);
    }
    return w;
}

// Deterministic blocked sum — REUSES the module's pinned-order reduction
// core (reduce.hpp detail::block_partial, SANITY #8): the summation tree is
// a fixed function of n ONLY (bit-reproducible with the same SIMD width).
template <typename T> [[nodiscard]] inline T block_sum(const T* v, crd::u64 n) noexcept
{
    return detail::block_partial<T, detail::RAdd<T>>(v, n, T(0));
}

// Vector max over n >= 1 values via the same core (RMax is idempotent, so
// v[0] doubling as the init is exact; comparison-based — order-independent
// bits for NaN-free data, the documented contract).
template <typename T> [[nodiscard]] inline T block_max(const T* v, crd::u64 n) noexcept
{
    return detail::block_partial<T, detail::RMax<T>>(v, n, v[0]);
}

// C-contiguous strides for a shape (row-major, elements)
inline void contiguous_strides(crd::containers::ConstSpan<crd::u64> shape, crd::u64* strides) noexcept
{
    crd::u64 s = 1;
    for (crd::u32 d = static_cast<crd::u32>(shape.size()); d-- > 0U;)
    {
        strides[d] = s;
        s *= shape[d];
    }
}

// tight rank-2 view check ([rows, cols] with stride(1)==1, stride(0)==cols)
template <typename T> [[nodiscard]] inline bool tight2(const TensorView<T>& v) noexcept
{
    return v.rank() == 2U && v.stride(1) == 1 && v.stride(0) == static_cast<crd::i64>(v.shape(1));
}

// forward declaration for the merge front-ends below
template <typename T> struct ScratchBlock;

// packed-key union front-end: worst-case fill + compaction (see merge_union)
template <typename T, typename KT>
[[nodiscard]] TensorStatus add_fast(const SparseCoo<T>& a, const SparseCoo<T>& b, SparseCoo<T>& out)
{
    const crd::u32 rank = a.rank();
    const crd::u64 na = a.nnz();
    const crd::u64 nb = b.nnz();
    const TensorStatus st = out.init(a.shape(), na + nb);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    ScratchBlock<KT> keys(out.allocator(), na + nb);
    if (na + nb > 0U && keys.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    KT* ka = keys.m_ptr;
    KT* kb = keys.m_ptr + na;
    pack_keys(a, ka);
    pack_keys(b, kb);
    crd::u32 shift[kMaxRank];
    crd::u64 mask[kMaxRank];
    (void)key_layout(a.shape(), shift, mask);
    crd::u32* op[kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        op[m] = out.idx_mut(m);
    }
    T* ov = out.val_mut();
    crd::u64 i = 0;
    crd::u64 j = 0;
    crd::u64 w = rank == 3U
                     ? merge_union<T, KT, 3U>(ka, na, a.val(), kb, nb, b.val(), rank, shift, mask, op, ov, i, j)
                     : merge_union<T, KT, 0U>(ka, na, a.val(), kb, nb, b.val(), rank, shift, mask, op, ov, i, j);
    if (i < na) // bulk tails: contiguous copies per plane
    {
        for (crd::u32 m = 0; m < rank; ++m)
        {
            std::memcpy(op[m] + w, a.idx(m) + i, static_cast<crd::usize>(na - i) * sizeof(crd::u32));
        }
        std::memcpy(ov + w, a.val() + i, static_cast<crd::usize>(na - i) * sizeof(T));
        w += na - i;
    }
    if (j < nb)
    {
        for (crd::u32 m = 0; m < rank; ++m)
        {
            std::memcpy(op[m] + w, b.idx(m) + j, static_cast<crd::usize>(nb - j) * sizeof(crd::u32));
        }
        std::memcpy(ov + w, b.val() + j, static_cast<crd::usize>(nb - j) * sizeof(T));
        w += nb - j;
    }
    out.compact_to(w);
    return TensorStatus::Ok;
}

// packed-key intersection front-end (see merge_intersect)
template <typename T, typename KT>
[[nodiscard]] TensorStatus mul_fast(const SparseCoo<T>& a, const SparseCoo<T>& b, SparseCoo<T>& out)
{
    const crd::u32 rank = a.rank();
    const crd::u64 na = a.nnz();
    const crd::u64 nb = b.nnz();
    const crd::u64 cap = na < nb ? na : nb;
    const TensorStatus st = out.init(a.shape(), cap);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    ScratchBlock<KT> keys(out.allocator(), na + nb);
    if (na + nb > 0U && keys.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    KT* ka = keys.m_ptr;
    KT* kb = keys.m_ptr + na;
    pack_keys(a, ka);
    pack_keys(b, kb);
    crd::u32 shift[kMaxRank];
    crd::u64 mask[kMaxRank];
    (void)key_layout(a.shape(), shift, mask);
    crd::u32* op[kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        op[m] = out.idx_mut(m);
    }
    T* ov = out.val_mut();
    const crd::u64 w =
        rank == 3U ? merge_intersect<T, KT, 3U>(ka, na, a.val(), kb, nb, b.val(), rank, shift, mask, op, ov)
                   : merge_intersect<T, KT, 0U>(ka, na, a.val(), kb, nb, b.val(), rank, shift, mask, op, ov);
    out.compact_to(w);
    return TensorStatus::Ok;
}

// RAII scratch block from an explicit IAllocator (status-checked by caller)
template <typename T> struct ScratchBlock
{
    ScratchBlock(crd::memory::IAllocator* alloc, crd::u64 count) noexcept : m_alloc(alloc)
    {
        if (count > 0U && alloc != nullptr)
        {
            m_ptr = static_cast<T*>(alloc->allocate(count * sizeof(T), 64U));
        }
    }
    ~ScratchBlock()
    {
        if (m_ptr != nullptr)
        {
            m_alloc->deallocate(m_ptr);
        }
    }
    ScratchBlock(const ScratchBlock&) = delete;
    ScratchBlock& operator=(const ScratchBlock&) = delete;
    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_ptr = nullptr;
};

} // namespace sparsedetail

// -----------------------------------------------------------------------
// SparseCooBuilder<T> — rank-N coordinate assembly (the TripletBuilder
// pattern): accumulate (idx..., value) in any order, then compress() to a
// canonical SparseCoo. Deterministic: stable LSD counting sort (insertion
// order within equal tuples) + left-to-right duplicate summation ⇒ compress
// is bit-reproducible for a fixed add() sequence.
// -----------------------------------------------------------------------
template <typename T> class SparseCooBuilder
{
public:
    SparseCooBuilder(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::u64> shape)
        : m_alloc(alloc), m_rank(static_cast<crd::u32>(shape.size())), m_idx(alloc), m_val(alloc)
    {
        CRD_ASSERT_MSG(alloc != nullptr, "SparseCooBuilder: null allocator");
        CRD_ASSERT_MSG(shape.size() >= 1U && shape.size() <= kMaxRank, "SparseCooBuilder: bad rank");
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            CRD_ASSERT_MSG(shape[m] <= 0xFFFFFFFFULL, "SparseCooBuilder: dim exceeds u32 index range");
            m_shape[m] = shape[m];
        }
    }

    void reserve(crd::usize n)
    {
        m_idx.reserve(n * m_rank);
        m_val.reserve(n);
    }

    void add(crd::containers::ConstSpan<crd::u32> idx, T v)
    {
        CRD_ASSERT_MSG(idx.size() == m_rank, "SparseCooBuilder::add: index rank mismatch");
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            CRD_ASSERT_MSG(idx[m] < m_shape[m], "SparseCooBuilder::add: index out of range");
            m_idx.push_back(idx[m]);
        }
        m_val.push_back(v);
    }

    [[nodiscard]] crd::usize triplet_count() const noexcept { return m_val.size(); }
    [[nodiscard]] crd::u32 rank() const noexcept { return m_rank; }

    // Canonicalize: lexicographic sort (mode 0 major), duplicates summed in
    // insertion order. `out` may use a different allocator than the builder.
    [[nodiscard]] TensorStatus compress(SparseCoo<T>& out) const
    {
        const crd::u64 n = m_val.size();
        if (n > 0xFFFFFFFFULL)
        {
            return TensorStatus::BadInput;
        }
        crd::u64 max_dim = 0;
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            max_dim = m_shape[m] > max_dim ? m_shape[m] : max_dim;
        }
        crd::containers::Array<crd::u32> perm_a(m_alloc);
        crd::containers::Array<crd::u32> perm_b(m_alloc);
        crd::containers::Array<crd::u32> counts(m_alloc);
        perm_a.resize_uninitialized(static_cast<crd::usize>(n));
        perm_b.resize_uninitialized(static_cast<crd::usize>(n));
        counts.resize_uninitialized(static_cast<crd::usize>(max_dim) + 1U);
        for (crd::u64 i = 0; i < n; ++i)
        {
            perm_a[static_cast<crd::usize>(i)] = static_cast<crd::u32>(i);
        }
        const crd::u32* raw = m_idx.data();
        const crd::u32 rank = m_rank;
        sparsedetail::lsd_sort(
            perm_a.data(), perm_b.data(), counts.data(), n, rank, m_shape,
            [raw, rank](crd::u32 item, crd::u32 level) noexcept
            { return raw[static_cast<crd::usize>(item) * rank + level]; });
        // count unique tuples
        crd::u64 unique = 0;
        for (crd::u64 i = 0; i < n; ++i)
        {
            if (i == 0U || !equal_tuples(raw, perm_a[static_cast<crd::usize>(i - 1U)],
                                         perm_a[static_cast<crd::usize>(i)]))
            {
                ++unique;
            }
        }
        const TensorStatus st = out.init({m_shape, m_rank}, unique);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        // fill with insertion-order duplicate summation
        crd::u64 w = 0;
        crd::u64 i = 0;
        while (i < n)
        {
            const crd::u32 first = perm_a[static_cast<crd::usize>(i)];
            T acc = m_val[first];
            crd::u64 j = i + 1U;
            while (j < n && equal_tuples(raw, first, perm_a[static_cast<crd::usize>(j)]))
            {
                acc = acc + m_val[perm_a[static_cast<crd::usize>(j)]];
                ++j;
            }
            for (crd::u32 m = 0; m < m_rank; ++m)
            {
                out.idx_mut(m)[w] = raw[static_cast<crd::usize>(first) * m_rank + m];
            }
            out.val_mut()[w] = acc;
            ++w;
            i = j;
        }
        return TensorStatus::Ok;
    }

private:
    [[nodiscard]] bool equal_tuples(const crd::u32* raw, crd::u32 a, crd::u32 b) const noexcept
    {
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            if (raw[static_cast<crd::usize>(a) * m_rank + m] != raw[static_cast<crd::usize>(b) * m_rank + m])
            {
                return false;
            }
        }
        return true;
    }

    crd::memory::IAllocator* m_alloc;
    crd::u32 m_rank;
    crd::u64 m_shape[kMaxRank] = {};
    crd::containers::Array<crd::u32> m_idx; // triplet-major [n][rank]
    crd::containers::Array<T> m_val;
};

// -----------------------------------------------------------------------
// SparseCsf<T> — compressed sparse fiber (the SPLATT mode tree): level l of
// the tree indexes tensor mode order(l); level-l nodes are the distinct
// (order(0..l)) index prefixes; fptr(l) spans each node's children at level
// l+1; leaf nodes align 1:1 with the values. Levels are stored concatenated
// (one allocation each for fids / fptr).
// -----------------------------------------------------------------------
template <typename T> class SparseCsf
{
public:
    explicit SparseCsf(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_fids(alloc), m_fptr(alloc), m_val(alloc)
    {
        CRD_ASSERT_MSG(alloc != nullptr, "SparseCsf: null allocator");
    }

    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }
    [[nodiscard]] crd::u32 rank() const noexcept { return m_rank; }
    [[nodiscard]] crd::u64 nnz() const noexcept { return m_nnz; }
    [[nodiscard]] crd::u64 shape(crd::u32 mode) const noexcept
    {
        CRD_ASSERT_MSG(mode < m_rank, "SparseCsf::shape: mode out of range");
        return m_shape[mode];
    }
    [[nodiscard]] crd::u64 size() const noexcept // logical element count
    {
        crd::u64 n = 1;
        for (crd::u32 m = 0; m < m_rank; ++m)
        {
            n *= m_shape[m];
        }
        return n;
    }
    [[nodiscard]] crd::u32 order(crd::u32 level) const noexcept
    {
        CRD_ASSERT_MSG(level < m_rank, "SparseCsf::order: level out of range");
        return m_order[level];
    }
    [[nodiscard]] crd::u64 nodes(crd::u32 level) const noexcept
    {
        CRD_ASSERT_MSG(level < m_rank, "SparseCsf::nodes: level out of range");
        return m_nodes[level];
    }
    [[nodiscard]] const crd::u32* fids(crd::u32 level) const noexcept
    {
        CRD_ASSERT_MSG(level < m_rank, "SparseCsf::fids: level out of range");
        return m_fids.data() + m_fids_off[level];
    }
    [[nodiscard]] const crd::u32* fptr(crd::u32 level) const noexcept
    {
        CRD_ASSERT_MSG(level + 1U < m_rank, "SparseCsf::fptr: level out of range");
        return m_fptr.data() + m_fptr_off[level];
    }
    [[nodiscard]] const T* val() const noexcept { return m_val.data(); }

    template <typename U> friend TensorStatus coo_to_csf(const SparseCoo<U>&, crd::containers::ConstSpan<crd::u32>,
                                                         SparseCsf<U>&);

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_rank = 0;
    crd::u64 m_shape[kMaxRank] = {};   // ORIGINAL tensor dims (mode-indexed)
    crd::u32 m_order[kMaxRank] = {};   // level -> mode
    crd::u64 m_nodes[kMaxRank] = {};   // node count per level (leaf == nnz)
    crd::u64 m_nnz = 0;
    crd::usize m_fids_off[kMaxRank + 1U] = {};
    crd::usize m_fptr_off[kMaxRank] = {};
    crd::containers::Array<crd::u32> m_fids;
    crd::containers::Array<crd::u32> m_fptr;
    crd::containers::Array<T> m_val;
};

// Build a CSF tree from a canonical COO. mode_order is the level->mode map
// (a permutation; order(0) = the root mode — MTTKRP's target mode). Scratch
// comes from the CSF's own allocator (propagated, never default_allocator).
template <typename T>
[[nodiscard]] TensorStatus coo_to_csf(const SparseCoo<T>& x, crd::containers::ConstSpan<crd::u32> mode_order,
                                      SparseCsf<T>& out)
{
    const crd::u32 rank = x.rank();
    if (rank == 0U || mode_order.size() != rank)
    {
        return TensorStatus::BadInput;
    }
    crd::u32 seen = 0;
    bool identity = true;
    for (crd::u32 l = 0; l < rank; ++l)
    {
        const crd::u32 m = mode_order[l];
        if (m >= rank || (seen & (1U << m)) != 0U)
        {
            return TensorStatus::BadInput;
        }
        seen |= 1U << m;
        identity = identity && m == l;
    }
    const crd::u64 n = x.nnz();
    out.m_rank = rank;
    out.m_nnz = n;
    for (crd::u32 m = 0; m < rank; ++m)
    {
        out.m_shape[m] = x.shape(m);
    }
    for (crd::u32 l = 0; l < rank; ++l)
    {
        out.m_order[l] = mode_order[l];
    }
    // enumeration order: identity = the canonical order; otherwise a stable
    // LSD re-sort by the permuted key sequence
    crd::containers::Array<crd::u32> perm_a(out.m_alloc);
    if (!identity)
    {
        crd::containers::Array<crd::u32> perm_b(out.m_alloc);
        crd::containers::Array<crd::u32> counts(out.m_alloc);
        crd::u64 level_dims[kMaxRank];
        crd::u64 max_dim = 0;
        for (crd::u32 l = 0; l < rank; ++l)
        {
            level_dims[l] = x.shape(mode_order[l]);
            max_dim = level_dims[l] > max_dim ? level_dims[l] : max_dim;
        }
        perm_a.resize_uninitialized(static_cast<crd::usize>(n));
        perm_b.resize_uninitialized(static_cast<crd::usize>(n));
        counts.resize_uninitialized(static_cast<crd::usize>(max_dim) + 1U);
        for (crd::u64 i = 0; i < n; ++i)
        {
            perm_a[static_cast<crd::usize>(i)] = static_cast<crd::u32>(i);
        }
        const crd::u32* planes[kMaxRank];
        for (crd::u32 l = 0; l < rank; ++l)
        {
            planes[l] = x.idx(mode_order[l]);
        }
        sparsedetail::lsd_sort(perm_a.data(), perm_b.data(), counts.data(), n, rank, level_dims,
                               [&planes](crd::u32 item, crd::u32 level) noexcept { return planes[level][item]; });
    }
    const auto ent = [&](crd::u64 e) noexcept -> crd::u64
    { return identity ? e : static_cast<crd::u64>(perm_a[static_cast<crd::usize>(e)]); };
    const crd::u32* lv[kMaxRank]; // level-ordered index planes
    for (crd::u32 l = 0; l < rank; ++l)
    {
        lv[l] = x.idx(mode_order[l]);
    }
    // pass 1: node counts per level (a node opens where any ancestor-or-self
    // level key changes)
    for (crd::u32 l = 0; l < rank; ++l)
    {
        out.m_nodes[l] = 0;
    }
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u32 d = rank; // first differing level vs previous entry
        if (e == 0U)
        {
            d = 0U;
        }
        else
        {
            const crd::u64 cur = ent(e);
            const crd::u64 prv = ent(e - 1U);
            for (crd::u32 l = 0; l < rank; ++l)
            {
                if (lv[l][cur] != lv[l][prv])
                {
                    d = l;
                    break;
                }
            }
        }
        for (crd::u32 l = d; l < rank; ++l)
        {
            ++out.m_nodes[l];
        }
    }
    // storage layout
    crd::usize fids_total = 0;
    for (crd::u32 l = 0; l < rank; ++l)
    {
        out.m_fids_off[l] = fids_total;
        fids_total += static_cast<crd::usize>(out.m_nodes[l]);
    }
    out.m_fids_off[rank] = fids_total;
    crd::usize fptr_total = 0;
    for (crd::u32 l = 0; l + 1U < rank; ++l)
    {
        out.m_fptr_off[l] = fptr_total;
        fptr_total += static_cast<crd::usize>(out.m_nodes[l]) + 1U;
    }
    out.m_fids.resize_uninitialized(fids_total);
    out.m_fptr.resize_uninitialized(fptr_total);
    out.m_val.resize_uninitialized(static_cast<crd::usize>(n));
    // pass 2: fill fids/fptr/values
    crd::u64 wcur[kMaxRank] = {}; // per-level node write cursor
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u32 d = rank;
        if (e == 0U)
        {
            d = 0U;
        }
        else
        {
            const crd::u64 cur = ent(e);
            const crd::u64 prv = ent(e - 1U);
            for (crd::u32 l = 0; l < rank; ++l)
            {
                if (lv[l][cur] != lv[l][prv])
                {
                    d = l;
                    break;
                }
            }
        }
        const crd::u64 cur = ent(e);
        for (crd::u32 l = d; l < rank; ++l)
        {
            out.m_fids.data()[out.m_fids_off[l] + static_cast<crd::usize>(wcur[l])] = lv[l][cur];
            if (l + 1U < rank)
            {
                out.m_fptr.data()[out.m_fptr_off[l] + static_cast<crd::usize>(wcur[l])] =
                    static_cast<crd::u32>(wcur[l + 1U]);
            }
            ++wcur[l];
        }
        out.m_val.data()[static_cast<crd::usize>(e)] = x.val()[cur];
    }
    for (crd::u32 l = 0; l + 1U < rank; ++l) // close sentinels
    {
        out.m_fptr.data()[out.m_fptr_off[l] + static_cast<crd::usize>(out.m_nodes[l])] =
            static_cast<crd::u32>(out.m_nodes[l + 1U]);
    }
    return TensorStatus::Ok;
}

// Reorder a canonical COO's modes: out mode l = x mode mode_order[l], then
// re-canonicalized (the sparse transpose; tuples stay unique so no dedup).
template <typename T>
[[nodiscard]] TensorStatus coo_reorder(const SparseCoo<T>& x, crd::containers::ConstSpan<crd::u32> mode_order,
                                       SparseCoo<T>& out)
{
    const crd::u32 rank = x.rank();
    if (rank == 0U || mode_order.size() != rank)
    {
        return TensorStatus::BadInput;
    }
    crd::u32 seen = 0;
    for (crd::u32 l = 0; l < rank; ++l)
    {
        const crd::u32 m = mode_order[l];
        if (m >= rank || (seen & (1U << m)) != 0U)
        {
            return TensorStatus::BadInput;
        }
        seen |= 1U << m;
    }
    const crd::u64 n = x.nnz();
    crd::u64 new_shape[kMaxRank];
    crd::u64 max_dim = 0;
    for (crd::u32 l = 0; l < rank; ++l)
    {
        new_shape[l] = x.shape(mode_order[l]);
        max_dim = new_shape[l] > max_dim ? new_shape[l] : max_dim;
    }
    const TensorStatus st = out.init({new_shape, rank}, n);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::containers::Array<crd::u32> perm_a(out.allocator());
    crd::containers::Array<crd::u32> perm_b(out.allocator());
    crd::containers::Array<crd::u32> counts(out.allocator());
    perm_a.resize_uninitialized(static_cast<crd::usize>(n));
    perm_b.resize_uninitialized(static_cast<crd::usize>(n));
    counts.resize_uninitialized(static_cast<crd::usize>(max_dim) + 1U);
    for (crd::u64 i = 0; i < n; ++i)
    {
        perm_a[static_cast<crd::usize>(i)] = static_cast<crd::u32>(i);
    }
    const crd::u32* planes[kMaxRank];
    for (crd::u32 l = 0; l < rank; ++l)
    {
        planes[l] = x.idx(mode_order[l]);
    }
    sparsedetail::lsd_sort(perm_a.data(), perm_b.data(), counts.data(), n, rank, new_shape,
                           [&planes](crd::u32 item, crd::u32 level) noexcept { return planes[level][item]; });
    for (crd::u32 l = 0; l < rank; ++l)
    {
        crd::u32* dst = out.idx_mut(l);
        const crd::u32* src = planes[l];
        for (crd::u64 e = 0; e < n; ++e)
        {
            dst[e] = src[perm_a[static_cast<crd::usize>(e)]];
        }
    }
    T* dv = out.val_mut();
    const T* sv = x.val();
    for (crd::u64 e = 0; e < n; ++e)
    {
        dv[e] = sv[perm_a[static_cast<crd::usize>(e)]];
    }
    return TensorStatus::Ok;
}

// -----------------------------------------------------------------------
// Sparse x dense mode-n contraction (TTM / mode-n product): out is the DENSE
// tensor with mode `mode` replaced by u's second dim —
//   out(i_0,..,f,..,i_{r-1}) = sum_{i_mode} x(i_0,..,i_mode,..,i_{r-1}) * u(i_mode, f)
// u = [shape(mode), F] tight row-major; out = C-contiguous, same shape as x
// with shape(mode) -> F. Deterministic at any worker count: mode != 0
// partitions by mode-0 fiber runs (disjoint out slices); mode == 0 uses a
// FIXED number of partial buffers (f(nnz/out-size only)) folded in task
// order.
// -----------------------------------------------------------------------
template <typename T>
[[nodiscard]] TensorStatus contract_mode(const SparseCoo<T>& x, crd::u32 mode, TensorView<const T> u,
                                         TensorView<T> out, crd::memory::IAllocator* scratch,
                                         crd::u32 num_workers = 0)
{
    using namespace sparsedetail;
    const crd::u32 rank = x.rank();
    if (rank == 0U || mode >= rank || !tight2(u) || u.shape(0) != x.shape(mode) || out.rank() != rank ||
        !out.is_contiguous() || scratch == nullptr)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 f = u.shape(1);
    for (crd::u32 m = 0; m < rank; ++m)
    {
        const crd::u64 want = m == mode ? f : x.shape(m);
        if (out.shape(m) != want)
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    const crd::u64 osize = out.size();
    T* pout = out.data();
    std::memset(pout, 0, static_cast<crd::usize>(osize) * sizeof(T));
    const crd::u64 n = x.nnz();
    if (n == 0U || osize == 0U || f == 0U)
    {
        return TensorStatus::Ok;
    }
    crd::u64 ostride[kMaxRank];
    contiguous_strides(out.shape(), ostride);
    const crd::u32* planes[kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        planes[m] = x.idx(m);
    }
    const T* pval = x.val();
    const T* pu = u.data();
    const crd::u64 osm = ostride[mode];
    // per-entry kernel: out[base + q*osm] += v * urow[q]
    const auto scatter_range = [&](crd::u64 eb, crd::u64 ee, T* dst) noexcept
    {
        for (crd::u64 e = eb; e < ee; ++e)
        {
            crd::u64 base = 0;
            for (crd::u32 m = 0; m < rank; ++m)
            {
                if (m != mode)
                {
                    base += static_cast<crd::u64>(planes[m][e]) * ostride[m];
                }
            }
            const T v = pval[e];
            const T* urow = pu + static_cast<crd::u64>(planes[mode][e]) * f;
            if (mode == rank - 1U)
            {
                axpy_row(dst + base, urow, v, f); // contiguous fastest path
            }
            else
            {
                T* o = dst + base;
                for (crd::u64 q = 0; q < f; ++q)
                {
                    o[q * osm] = std::fma(v, urow[q], o[q * osm]);
                }
            }
        }
    };
    // staged tile path for an INNER contracted mode (0 < mode < rank-1) with
    // an affordable [suffix, F] tile: per PREFIX run (modes 0..mode-1
    // constant), per-entry CONTIGUOUS axpy into the tile, then one blocked
    // transpose into the contiguous out slice — replaces the F-line strided
    // scatter per nonzero. Per-out-element fma chains stay in the exact same
    // e-order as the scatter kernel (bit-identical paths).
    const crd::u64 suffix = ostride[mode]; // product of the trailing out dims
    const bool staged =
        mode != 0U && mode + 1U < rank && suffix * f * sizeof(T) <= 8ULL * 1024ULL * 1024ULL;
    const auto staged_range = [&](crd::u64 eb, crd::u64 ee, T* tile) noexcept
    {
        crd::u64 e = eb;
        while (e < ee)
        {
            crd::u64 re = e + 1U; // prefix-run end (modes 0..mode-1 constant)
            while (re < ee)
            {
                bool same = true;
                for (crd::u32 m = 0; m < mode; ++m)
                {
                    if (planes[m][re] != planes[m][e])
                    {
                        same = false;
                        break;
                    }
                }
                if (!same)
                {
                    break;
                }
                ++re;
            }
            crd::u64 pbase = 0;
            for (crd::u32 m = 0; m < mode; ++m)
            {
                pbase += static_cast<crd::u64>(planes[m][e]) * ostride[m];
            }
            std::memset(tile, 0, static_cast<crd::usize>(suffix * f) * sizeof(T));
            for (crd::u64 e2 = e; e2 < re; ++e2)
            {
                crd::u64 soff = 0;
                for (crd::u32 m = mode + 1U; m < rank; ++m)
                {
                    soff += static_cast<crd::u64>(planes[m][e2]) * ostride[m];
                }
                axpy_row(tile + soff * f, pu + static_cast<crd::u64>(planes[mode][e2]) * f, pval[e2], f);
            }
            T* oslice = pout + pbase; // blocked transpose tile[s][q] -> out[q][s]
            constexpr crd::u64 bs = 128;
            for (crd::u64 s0 = 0; s0 < suffix; s0 += bs)
            {
                const crd::u64 s1 = suffix - s0 < bs ? suffix : s0 + bs;
                for (crd::u64 q = 0; q < f; ++q)
                {
                    T* orow = oslice + q * suffix;
                    for (crd::u64 s = s0; s < s1; ++s)
                    {
                        orow[s] = tile[s * f + q];
                    }
                }
            }
            e = re;
        }
    };
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (mode != 0U)
    {
        // mode-0 fiber runs = disjoint out slices (out keeps the mode-0
        // index; every colliding output group lives inside ONE mode-0 run)
        const crd::u32 tasks = task_count(n, 2ULL * f, staged ? 64U : 256U);
        ScratchBlock<T> tiles(scratch, staged ? static_cast<crd::u64>(tasks) * suffix * f : 0U);
        if (staged && tiles.m_ptr == nullptr)
        {
            return TensorStatus::AllocFailed;
        }
        struct Ctx
        {
            const decltype(scatter_range)* scatter;
            const decltype(staged_range)* stage;
            T* tiles;
            crd::u64 tile_elems;
            const crd::u32* root;
            T* out;
            crd::u64 n;
            crd::u32 tasks;
            bool staged;
        };
        Ctx ctx{&scatter_range, &staged_range, tiles.m_ptr, suffix * f, planes[0], pout, n, tasks, staged};
        Ctx* const cp = &ctx;
        const auto run_tasks = [cp](crd::u32 tb, crd::u32 te) noexcept
        {
            for (crd::u32 t = tb; t < te; ++t)
            {
                crd::u64 eb = cp->n * t / cp->tasks;
                crd::u64 ee = cp->n * (t + 1U) / cp->tasks;
                while (eb > 0U && eb < cp->n && cp->root[eb] == cp->root[eb - 1U])
                {
                    ++eb; // snap forward to a mode-0 run boundary
                }
                while (ee > 0U && ee < cp->n && cp->root[ee] == cp->root[ee - 1U])
                {
                    ++ee;
                }
                if (eb >= ee)
                {
                    continue;
                }
                if (cp->staged)
                {
                    (*cp->stage)(eb, ee, cp->tiles + static_cast<crd::u64>(t) * cp->tile_elems);
                }
                else
                {
                    (*cp->scatter)(eb, ee, cp->out);
                }
            }
        };
        if (nw <= 1U || tasks < 2U)
        {
            run_tasks(0U, tasks);
            return TensorStatus::Ok;
        }
        struct CtxR
        {
            const decltype(run_tasks)* run;
        };
        CtxR ctxr{&run_tasks};
        CtxR* const rp = &ctxr;
        auto* const counter =
            crd::jobs::parallel_for(tasks, nw, [rp](crd::u32 tb, crd::u32 te) { (*rp->run)(tb, te); });
        crd::jobs::wait(counter);
        return TensorStatus::Ok;
    }
    // mode == 0: colliding outputs across the whole range — fixed partial
    // buffers (count = f(nnz, out bytes) ONLY), folded into out in task order.
    crd::u64 nparts64 = task_count(n, 2ULL * f, 8U);
    const crd::u64 budget = 256ULL * 1024ULL * 1024ULL; // partial-buffer memory cap
    const crd::u64 by_mem = budget / (osize * sizeof(T) + 1ULL);
    if (nparts64 > by_mem)
    {
        nparts64 = by_mem;
    }
    if (nparts64 < 1U)
    {
        nparts64 = 1U;
    }
    const crd::u32 nparts = static_cast<crd::u32>(nparts64);
    if (nparts < 2U)
    {
        scatter_range(0U, n, pout);
        return TensorStatus::Ok;
    }
    // nparts >= 2: ALWAYS take the partial-buffer path (even serially) — the
    // summation grouping is then a function of shape/nnz only, never of the
    // live worker count (the moat contract).
    ScratchBlock<T> partials(scratch, static_cast<crd::u64>(nparts) * osize);
    if (partials.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    struct Ctx0
    {
        const decltype(scatter_range)* scatter;
        T* parts;
        crd::u64 osize;
        crd::u64 n;
        crd::u32 nparts;
    };
    Ctx0 ctx{&scatter_range, partials.m_ptr, osize, n, nparts};
    Ctx0* const cp = &ctx;
    const auto run_part = [cp](crd::u32 tb, crd::u32 te) noexcept
    {
        for (crd::u32 t = tb; t < te; ++t)
        {
            T* buf = cp->parts + static_cast<crd::u64>(t) * cp->osize;
            std::memset(buf, 0, static_cast<crd::usize>(cp->osize) * sizeof(T));
            const crd::u64 eb = cp->n * t / cp->nparts;
            const crd::u64 ee = cp->n * (t + 1U) / cp->nparts;
            (*cp->scatter)(eb, ee, buf);
        }
    };
    if (nw <= 1U)
    {
        run_part(0U, nparts);
    }
    else
    {
        struct CtxR
        {
            const decltype(run_part)* run;
        };
        CtxR ctxr{&run_part};
        CtxR* const rp = &ctxr;
        auto* const counter =
            crd::jobs::parallel_for(nparts, nw, [rp](crd::u32 tb, crd::u32 te) { (*rp->run)(tb, te); });
        crd::jobs::wait(counter);
    }
    for (crd::u32 t = 0; t < nparts; ++t) // fixed-order fold
    {
        sparsedetail::add_acc(pout, partials.m_ptr + static_cast<crd::u64>(t) * osize, osize);
    }
    return TensorStatus::Ok;
}

// -----------------------------------------------------------------------
// Sparse elementwise + conversions
// -----------------------------------------------------------------------

// out = a + b: structural UNION of two canonical same-shape tensors (torch
// sparse add-then-coalesce semantics); overlapping entries sum a + b.
template <typename T>
[[nodiscard]] TensorStatus sparse_add(const SparseCoo<T>& a, const SparseCoo<T>& b, SparseCoo<T>& out)
{
    using namespace sparsedetail;
    if (a.rank() != b.rank() || a.rank() == 0U)
    {
        return TensorStatus::BadInput;
    }
    for (crd::u32 m = 0; m < a.rank(); ++m)
    {
        if (a.shape(m) != b.shape(m))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    const crd::u32 rank = a.rank();
    const crd::u64 na = a.nnz();
    const crd::u64 nb = b.nnz();
    crd::u32 total_bits = 0;
    for (crd::u32 m = 0; m < rank; ++m)
    {
        total_bits += dim_bits(a.shape(m));
    }
    if (total_bits <= 32U)
    {
        return sparsedetail::add_fast<T, crd::u32>(a, b, out);
    }
    if (total_bits <= 64U)
    {
        return sparsedetail::add_fast<T, crd::u64>(a, b, out);
    }
    // general fallback: two-pass tuple-compare merge (keys wider than 64 bits)
    crd::u64 count = 0; // pass 1: union size
    {
        crd::u64 i = 0;
        crd::u64 j = 0;
        while (i < na && j < nb)
        {
            const crd::i32 c = cmp_tuple(a, i, b, j);
            i += c <= 0 ? 1U : 0U;
            j += c >= 0 ? 1U : 0U;
            ++count;
        }
        count += (na - i) + (nb - j);
    }
    const TensorStatus st = out.init(a.shape(), count);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::u64 i = 0;
    crd::u64 j = 0;
    crd::u64 w = 0;
    while (i < na || j < nb)
    {
        crd::i32 c;
        if (i >= na)
        {
            c = 1;
        }
        else if (j >= nb)
        {
            c = -1;
        }
        else
        {
            c = cmp_tuple(a, i, b, j);
        }
        for (crd::u32 m = 0; m < rank; ++m)
        {
            out.idx_mut(m)[w] = c <= 0 ? a.idx(m)[i] : b.idx(m)[j];
        }
        if (c == 0)
        {
            out.val_mut()[w] = a.val()[i] + b.val()[j];
            ++i;
            ++j;
        }
        else if (c < 0)
        {
            out.val_mut()[w] = a.val()[i];
            ++i;
        }
        else
        {
            out.val_mut()[w] = b.val()[j];
            ++j;
        }
        ++w;
    }
    return TensorStatus::Ok;
}

// out = a * b (Hadamard): structural INTERSECTION of two canonical
// same-shape tensors.
template <typename T>
[[nodiscard]] TensorStatus sparse_mul(const SparseCoo<T>& a, const SparseCoo<T>& b, SparseCoo<T>& out)
{
    using namespace sparsedetail;
    if (a.rank() != b.rank() || a.rank() == 0U)
    {
        return TensorStatus::BadInput;
    }
    for (crd::u32 m = 0; m < a.rank(); ++m)
    {
        if (a.shape(m) != b.shape(m))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    const crd::u32 rank = a.rank();
    const crd::u64 na = a.nnz();
    const crd::u64 nb = b.nnz();
    crd::u32 total_bits = 0;
    for (crd::u32 m = 0; m < rank; ++m)
    {
        total_bits += dim_bits(a.shape(m));
    }
    if (total_bits <= 32U)
    {
        return sparsedetail::mul_fast<T, crd::u32>(a, b, out);
    }
    if (total_bits <= 64U)
    {
        return sparsedetail::mul_fast<T, crd::u64>(a, b, out);
    }
    // general fallback: two-pass tuple-compare intersection
    crd::u64 count = 0;
    {
        crd::u64 i = 0;
        crd::u64 j = 0;
        while (i < na && j < nb)
        {
            const crd::i32 c = cmp_tuple(a, i, b, j);
            count += c == 0 ? 1U : 0U;
            i += c <= 0 ? 1U : 0U;
            j += c >= 0 ? 1U : 0U;
        }
    }
    const TensorStatus st = out.init(a.shape(), count);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::u64 i = 0;
    crd::u64 j = 0;
    crd::u64 w = 0;
    while (i < na && j < nb)
    {
        const crd::i32 c = cmp_tuple(a, i, b, j);
        if (c == 0)
        {
            for (crd::u32 m = 0; m < rank; ++m)
            {
                out.idx_mut(m)[w] = a.idx(m)[i];
            }
            out.val_mut()[w] = a.val()[i] * b.val()[j];
            ++w;
            ++i;
            ++j;
        }
        else if (c < 0)
        {
            ++i;
        }
        else
        {
            ++j;
        }
    }
    return TensorStatus::Ok;
}

// out = a * d (Hadamard with a DENSE operand, any strides): pattern of a,
// values scaled by the dense element at each coordinate.
template <typename T>
[[nodiscard]] TensorStatus sparse_mul_dense(const SparseCoo<T>& a, TensorView<const T> d, SparseCoo<T>& out)
{
    if (d.rank() != a.rank() || a.rank() == 0U)
    {
        return TensorStatus::BadInput;
    }
    for (crd::u32 m = 0; m < a.rank(); ++m)
    {
        if (d.shape(m) != a.shape(m))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    const crd::u64 n = a.nnz();
    const TensorStatus st = out.init(a.shape(), n);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    const T* pd = d.data();
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::i64 off = 0;
        for (crd::u32 m = 0; m < a.rank(); ++m)
        {
            out.idx_mut(m)[e] = a.idx(m)[e];
            off += static_cast<crd::i64>(a.idx(m)[e]) * d.stride(m);
        }
        out.val_mut()[e] = a.val()[e] * pd[off];
    }
    return TensorStatus::Ok;
}

// out = d + a (DENSE result): copy the dense operand, then scatter-add the
// sparse values in canonical order. out must be C-contiguous, same shape.
template <typename T>
[[nodiscard]] TensorStatus sparse_add_dense(const SparseCoo<T>& a, TensorView<const T> d, TensorView<T> out)
{
    using namespace sparsedetail;
    if (d.rank() != a.rank() || out.rank() != a.rank() || a.rank() == 0U || !out.is_contiguous())
    {
        return TensorStatus::BadInput;
    }
    for (crd::u32 m = 0; m < a.rank(); ++m)
    {
        if (d.shape(m) != a.shape(m) || out.shape(m) != a.shape(m))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    T* pout = out.data();
    if (d.is_contiguous())
    {
        std::memcpy(pout, d.data(), static_cast<crd::usize>(out.size()) * sizeof(T));
    }
    else
    {
        crd::u64 w = 0;
        d.for_each([&](const crd::u64*, const T& v) { pout[w++] = v; });
    }
    crd::u64 ostride[kMaxRank];
    contiguous_strides(out.shape(), ostride);
    const crd::u64 n = a.nnz();
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u64 off = 0;
        for (crd::u32 m = 0; m < a.rank(); ++m)
        {
            off += static_cast<crd::u64>(a.idx(m)[e]) * ostride[m];
        }
        pout[off] = pout[off] + a.val()[e];
    }
    return TensorStatus::Ok;
}

// Densify: out (C-contiguous, same shape) = zeros + scatter.
template <typename T>
[[nodiscard]] TensorStatus to_dense(const SparseCoo<T>& a, TensorView<T> out)
{
    using namespace sparsedetail;
    if (out.rank() != a.rank() || a.rank() == 0U || !out.is_contiguous())
    {
        return TensorStatus::BadInput;
    }
    for (crd::u32 m = 0; m < a.rank(); ++m)
    {
        if (out.shape(m) != a.shape(m))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    T* pout = out.data();
    std::memset(pout, 0, static_cast<crd::usize>(out.size()) * sizeof(T));
    crd::u64 ostride[kMaxRank];
    contiguous_strides(out.shape(), ostride);
    const crd::u64 n = a.nnz();
    for (crd::u64 e = 0; e < n; ++e)
    {
        crd::u64 off = 0;
        for (crd::u32 m = 0; m < a.rank(); ++m)
        {
            off += static_cast<crd::u64>(a.idx(m)[e]) * ostride[m];
        }
        pout[off] = a.val()[e];
    }
    return TensorStatus::Ok;
}

// -----------------------------------------------------------------------
// Reductions — DENSE semantics (a densified array's result): sums touch only
// stored values (implicit zeros add 0); max compares implicit zeros wherever
// a slice is not fully dense. Serial and deterministic: the blocked-vector
// summation tree is a fixed function of nnz ONLY (bit-reproducible); max is
// order-independent on NaN-free data (comparison-based, NaN-unaware —
// documented, matching the sparse peers).
// -----------------------------------------------------------------------

template <typename T> [[nodiscard]] TensorStatus reduce_sum(const SparseCoo<T>& a, T& out) noexcept
{
    if (a.rank() == 0U)
    {
        return TensorStatus::BadInput;
    }
    // deterministic blocked vector sum — the summation tree is a fixed
    // function of nnz only (bit-reproducible)
    out = sparsedetail::block_sum(a.val(), a.nnz());
    return TensorStatus::Ok;
}

// out[i] = sum over the mode-`mode` slice i (out.size() == shape(mode)).
template <typename T>
[[nodiscard]] TensorStatus reduce_sum_mode(const SparseCoo<T>& a, crd::u32 mode,
                                           crd::containers::Span<T> out) noexcept
{
    if (a.rank() == 0U || mode >= a.rank() || out.size() != a.shape(mode))
    {
        return TensorStatus::BadInput;
    }
    std::memset(out.data(), 0, out.size() * sizeof(T));
    const crd::u32* plane = a.idx(mode);
    const T* v = a.val();
    const crd::u64 n = a.nnz();
    T* po = out.data();
    if (mode == 0U)
    {
        // canonical order groups mode 0 into contiguous runs: one blocked
        // vector sum per run (no dependent scalar scatter chain)
        crd::u64 e = 0;
        while (e < n)
        {
            const crd::u32 i = plane[e];
            crd::u64 e2 = e + 1U;
            while (e2 < n && plane[e2] == i)
            {
                ++e2;
            }
            po[i] = sparsedetail::block_sum(v + e, e2 - e);
            e = e2;
        }
        return TensorStatus::Ok;
    }
    for (crd::u64 e = 0; e < n; ++e)
    {
        po[plane[e]] = po[plane[e]] + v[e];
    }
    return TensorStatus::Ok;
}

template <typename T> [[nodiscard]] TensorStatus reduce_max(const SparseCoo<T>& a, T& out) noexcept
{
    if (a.rank() == 0U || a.size() == 0U)
    {
        return TensorStatus::BadInput; // max of an empty array is undefined (numpy errors)
    }
    const crd::u64 n = a.nnz();
    T best = n > 0U ? sparsedetail::block_max(a.val(), n) : T(0);
    if (n < a.size() && (n == 0U || T(0) > best)) // implicit zeros participate
    {
        best = T(0);
    }
    out = best;
    return TensorStatus::Ok;
}

// out[i] = max over the mode-`mode` slice i, dense semantics (slices with any
// implicit zero include 0; an all-empty slice is exactly 0). scratch holds
// the per-slice nnz counters (shape(mode) u32 entries).
template <typename T>
[[nodiscard]] TensorStatus reduce_max_mode(const SparseCoo<T>& a, crd::u32 mode, crd::containers::Span<T> out,
                                           crd::memory::IAllocator* scratch)
{
    if (a.rank() == 0U || mode >= a.rank() || out.size() != a.shape(mode) || scratch == nullptr ||
        a.size() == 0U)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 dim = a.shape(mode);
    const crd::u64 slice = a.size() / dim;
    const crd::u32* plane = a.idx(mode);
    const T* v = a.val();
    const crd::u64 n = a.nnz();
    T* po = out.data();
    if (mode == 0U)
    {
        // run-wise vector max over the canonical mode-0 grouping; untouched
        // slices are all-implicit-zero (= 0); no counter scratch needed
        std::memset(po, 0, static_cast<crd::usize>(dim) * sizeof(T));
        crd::u64 e = 0;
        while (e < n)
        {
            const crd::u32 i = plane[e];
            crd::u64 e2 = e + 1U;
            while (e2 < n && plane[e2] == i)
            {
                ++e2;
            }
            T best = sparsedetail::block_max(v + e, e2 - e);
            if (e2 - e < slice && T(0) > best) // implicit zero participates
            {
                best = T(0);
            }
            po[i] = best;
            e = e2;
        }
        return TensorStatus::Ok;
    }
    sparsedetail::ScratchBlock<crd::u32> cnt(scratch, dim);
    if (cnt.m_ptr == nullptr)
    {
        return TensorStatus::AllocFailed;
    }
    std::memset(cnt.m_ptr, 0, static_cast<crd::usize>(dim) * sizeof(crd::u32));
    for (crd::u64 e = 0; e < n; ++e)
    {
        const crd::u32 i = plane[e];
        if (cnt.m_ptr[i] == 0U || v[e] > po[i])
        {
            po[i] = v[e];
        }
        ++cnt.m_ptr[i];
    }
    for (crd::u64 i = 0; i < dim; ++i)
    {
        if (static_cast<crd::u64>(cnt.m_ptr[i]) < slice) // implicit zero present
        {
            if (cnt.m_ptr[i] == 0U || T(0) > po[i])
            {
                po[i] = T(0);
            }
        }
    }
    return TensorStatus::Ok;
}

// -----------------------------------------------------------------------
// CSF ROOT-mode reductions: the tree's fptr chain hands each root fiber's
// CONTIGUOUS value range over directly (scipy-csr-style precomputed
// boundaries — no index-plane run detection). Reduce over mode order(0);
// same dense semantics and blocked-deterministic order as the COO forms.
// -----------------------------------------------------------------------

template <typename T> [[nodiscard]] TensorStatus reduce_sum(const SparseCsf<T>& x, T& out) noexcept
{
    if (x.rank() == 0U)
    {
        return TensorStatus::BadInput;
    }
    out = sparsedetail::block_sum(x.val(), x.nnz());
    return TensorStatus::Ok;
}

template <typename T>
[[nodiscard]] TensorStatus reduce_sum_root(const SparseCsf<T>& x, crd::containers::Span<T> out) noexcept
{
    const crd::u32 rank = x.rank();
    if (rank == 0U || out.size() != x.shape(x.order(0)))
    {
        return TensorStatus::BadInput;
    }
    std::memset(out.data(), 0, out.size() * sizeof(T));
    const crd::u64 n0 = x.nodes(0);
    const crd::u32* fids0 = n0 > 0U ? x.fids(0) : nullptr;
    const T* v = x.val();
    T* po = out.data();
    crd::u64 b = 0;
    for (crd::u64 z0 = 0; z0 < n0; ++z0)
    {
        crd::u64 eb = z0 + 1U; // chase the next root's boundary to leaf depth
        for (crd::u32 l = 0; l + 1U < rank; ++l)
        {
            eb = x.fptr(l)[eb];
        }
        po[fids0[z0]] = sparsedetail::block_sum(v + b, eb - b);
        b = eb;
    }
    return TensorStatus::Ok;
}

template <typename T>
[[nodiscard]] TensorStatus reduce_max_root(const SparseCsf<T>& x, crd::containers::Span<T> out) noexcept
{
    const crd::u32 rank = x.rank();
    if (rank == 0U || out.size() != x.shape(x.order(0)) || x.size() == 0U)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 dim = x.shape(x.order(0));
    const crd::u64 slice = x.size() / dim;
    std::memset(out.data(), 0, out.size() * sizeof(T)); // untouched roots = all-implicit-zero slices
    const crd::u64 n0 = x.nodes(0);
    const crd::u32* fids0 = n0 > 0U ? x.fids(0) : nullptr;
    const T* v = x.val();
    T* po = out.data();
    crd::u64 b = 0;
    for (crd::u64 z0 = 0; z0 < n0; ++z0)
    {
        crd::u64 eb = z0 + 1U;
        for (crd::u32 l = 0; l + 1U < rank; ++l)
        {
            eb = x.fptr(l)[eb];
        }
        T best = sparsedetail::block_max(v + b, eb - b);
        if (eb - b < slice && T(0) > best) // implicit zero participates
        {
            best = T(0);
        }
        po[fids0[z0]] = best;
        b = eb;
    }
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
