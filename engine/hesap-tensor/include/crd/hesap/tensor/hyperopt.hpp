#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-g: the cotengra-class contraction-tree HYPER-OPTIMIZER
// for LARGE tensor networks (beyond einsum.hpp's 16-operand/34-index bitmask
// tier): random-greedy trials + label-propagation divisive partition trees +
// treesa simulated annealing + subtree-reconfigure (exact subset-DP re-solve)
// + SliceFinder dynamic slicing with an EXACTLY-honored memory bound (the
// ADR-0095 WCET pillar applied to einsum).
//
// Faithful to the cotengra 0.8.2 algorithm family, reconstructed and verified
// in python FIRST (scripts/v14g_hyperopt_recon{,2}.py; boards:
// docs/bench/2026-07-05-v14g-hyperopt-oracle.md — cost model bit-match 6/6,
// T=0 greedy identical, matched-tree slicer parity, quality 5W/1T/0L vs
// cotengra greedy+kahypar at matched 64-trial budgets).
//
// Determinism (the moat): every stochastic draw comes from PhiloxRng keyed
// (seed, trial-stream) — reproducible-by-seed at ANY worker count; trials are
// the parallel unit. cotengra's default stack (unseeded TPE, global-RNG trial
// fns, completion-order feedback) is non-reproducible by construction.
//
// Cost domains: per-step flops/sizes in u64 with STICKY SATURATION (planner
// stays well-defined on absurd networks; realistic boards fit easily — rand200
// peaks ~2^42 flops); totals in f64 (exact as integers below 2^53, monotone
// above — ranking is what matters). The slicer's incremental reductions use
// exact u64 floor division like the reference.
//
// The subtree re-solve uses a LOCAL subset-DP over pooled legs rather than
// einsum.hpp's optimal_iterate: that DP is bound to the 34-index u64 bitmask
// EinsumExpr universe, while hyperopt indices are unbounded u32 ids (checked
// per SANITY #8: reuse examined, representation incompatible — the ≤10-leaf
// DP is re-expressed here over the pooled-leg merge primitive instead).
// ---------------------------------------------------------------------------
#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <bit>
#include <cmath>

namespace crd::hesap::tensor
{

enum class HyperStatus : crd::u8
{
    Ok,
    BadInput,
    AllocFailed,
    NotFound, // e.g. slicing target unreachable under the forbidden set
};

// One index leg on a node: interned index id + its appearance count within the
// node's (sub)term. An index is dropped from a contraction result exactly when
// its accumulated count reaches its global appearance count.
struct HyperLeg
{
    crd::u32 ix;
    crd::u32 count;
};

// One pairwise contraction step in SSA form (leaves 0..n-1; step k's result is
// node n+k). flops is the cotengra metric: product over the UNION of the two
// operands' index dims (no multiply-add factor).
struct HyperStep
{
    crd::u32 a;
    crd::u32 b;
    crd::u64 flops;    // saturating
    crd::u64 out_size; // saturating
};

struct HyperPlan
{
    crd::containers::Array<HyperStep> steps;
    crd::f64 total_flops = 0.0;
    crd::u64 max_size = 1; // largest intermediate (elements, saturating)
    crd::u32 method = 0;   // which trial family produced it (HyperMethod)
    explicit HyperPlan(crd::memory::IAllocator* alloc) : steps(alloc) {}
};

namespace hyperdetail
{

inline constexpr crd::u64 kSat = ~crd::u64{0};

[[nodiscard]] inline crd::u64 sat_mul(crd::u64 a, crd::u64 b) noexcept
{
    if (a == 0U || b == 0U)
    {
        return 0U;
    }
    if (a > kSat / b)
    {
        return kSat;
    }
    return a * b;
}

// ---------------------------------------------------------------------------
// The contraction network: dense SSA node table (lazy-dead), pooled legs,
// per-index incident-node lists with lazy deletion. Deterministic iteration
// everywhere (arrays only — no hash-order dependence).
// ---------------------------------------------------------------------------
class HyperNet
{
public:
    explicit HyperNet(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_leg_pool(alloc), m_node_off(alloc), m_node_len(alloc), m_node_alive(alloc),
          m_edge_off(alloc), m_edge_len(alloc), m_edge_cap(alloc), m_edge_nodes(alloc), m_appear(alloc),
          m_sizes(alloc), m_ssa_a(alloc), m_ssa_b(alloc)
    {
    }

    // Build from operand index lists. ids[t] spans term t's index ids (repeats
    // allowed = diagonals pre-resolved upstream); sizes[ix] = dim of index ix;
    // out_ids = the output (open) indices. Size-1 indices are skipped entirely
    // (reference behaviour). Returns BadInput on malformed ids.
    HyperStatus build(crd::containers::ConstSpan<crd::containers::ConstSpan<crd::u32>> ids,
                      crd::containers::ConstSpan<crd::u32> out_ids,
                      crd::containers::ConstSpan<crd::u64> sizes) noexcept;

    [[nodiscard]] crd::u32 n_leaves() const noexcept { return m_n_leaves; }
    [[nodiscard]] crd::u32 n_alive() const noexcept { return m_n_alive; }
    [[nodiscard]] crd::u32 ssa_next() const noexcept { return static_cast<crd::u32>(m_node_off.size()); }
    [[nodiscard]] bool alive(crd::u32 i) const noexcept { return m_node_alive[i] != 0U; }
    [[nodiscard]] crd::f64 total_flops() const noexcept { return m_flops; }

    [[nodiscard]] crd::containers::ConstSpan<HyperLeg> legs(crd::u32 i) const noexcept
    {
        return {m_leg_pool.data() + m_node_off[i], m_node_len[i]};
    }

    [[nodiscard]] crd::u64 size_of(crd::containers::ConstSpan<HyperLeg> legs) const noexcept
    {
        crd::u64 s = 1;
        for (const HyperLeg& l : legs)
        {
            s = sat_mul(s, m_sizes[l.ix]);
        }
        return s;
    }

    // Product over the UNION of the two operands' indices (the cotengra flop
    // metric) — two-pointer over the sorted leg lists.
    [[nodiscard]] crd::u64 flops_of(crd::containers::ConstSpan<HyperLeg> a,
                                    crd::containers::ConstSpan<HyperLeg> b) const noexcept;

    // Sorted two-pointer merge; a shared index keeps count a+b and is dropped
    // exactly when that reaches its global appearance count. Appends the merged
    // legs to the pool and returns their span (pool only grows — planner arena).
    crd::u32 merge_legs(crd::containers::ConstSpan<HyperLeg> a, crd::containers::ConstSpan<HyperLeg> b,
                        crd::u32& out_off) noexcept;

    // Contract alive nodes i and j; records the SSA pair + flops; returns the
    // new node id (or ~0u on allocation failure).
    crd::u32 contract(crd::u32 i, crd::u32 j) noexcept;

    // Iterate the alive neighbours of node i reachable through edges incident
    // to at most max_neighbors nodes (the batch-index guard). Deterministic
    // order: legs in sorted-ix order, edge lists in insertion order. Calls
    // fn(j) for each (duplicates possible, exactly like the reference).
    template <typename Fn>
    void for_neighbors_limit(crd::u32 i, crd::u32 max_neighbors, Fn&& fn) const noexcept
    {
        for (const HyperLeg& l : legs(i))
        {
            const crd::u32 len = m_edge_len[l.ix];
            if (len > max_neighbors)
            {
                continue;
            }
            const crd::u32* ns = m_edge_nodes.data() + m_edge_off[l.ix];
            for (crd::u32 k = 0; k < m_edge_cap[l.ix]; ++k)
            {
                const crd::u32 j = ns[k];
                if (j != kDead && j != i && m_node_alive[j] != 0U)
                {
                    fn(j);
                }
            }
        }
    }

    [[nodiscard]] crd::u64 index_size(crd::u32 ix) const noexcept { return m_sizes[ix]; }
    [[nodiscard]] crd::u32 n_indices() const noexcept { return static_cast<crd::u32>(m_sizes.size()); }
    [[nodiscard]] crd::u32 appearances(crd::u32 ix) const noexcept { return m_appear[ix]; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // Iterate the alive members of index ix's edge in slab (insertion) order,
    // skipping the edge entirely when it exceeds the batch guard.
    template <typename Fn>
    void for_edge_members(crd::u32 ix, crd::u32 max_neighbors, Fn&& fn) const noexcept
    {
        if (m_edge_len[ix] > max_neighbors)
        {
            return;
        }
        const crd::u32* ns = m_edge_nodes.data() + m_edge_off[ix];
        for (crd::u32 k = 0; k < m_edge_cap[ix]; ++k)
        {
            if (ns[k] != kDead && m_node_alive[ns[k]] != 0U)
            {
                fn(ns[k]);
            }
        }
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> ssa_a() const noexcept { return {m_ssa_a.data(), m_ssa_a.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> ssa_b() const noexcept { return {m_ssa_b.data(), m_ssa_b.size()}; }

    // Deep copy for per-trial replay (pool/offsets/edges duplicated; index
    // metadata shared via spans is copied too — trials own their state).
    [[nodiscard]] HyperStatus clone_from(const HyperNet& src) noexcept;

    static constexpr crd::u32 kDead = ~crd::u32{0};

private:
    crd::u32 add_node(crd::u32 off, crd::u32 len) noexcept;
    void kill_node(crd::u32 i) noexcept;
    bool edge_push(crd::u32 ix, crd::u32 node) noexcept;

    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<HyperLeg> m_leg_pool;
    crd::containers::Array<crd::u32> m_node_off;
    crd::containers::Array<crd::u32> m_node_len;
    crd::containers::Array<crd::u8> m_node_alive;
    crd::containers::Array<crd::u32> m_edge_off;   // per index: offset into m_edge_nodes
    crd::containers::Array<crd::u32> m_edge_len;   // alive count
    crd::containers::Array<crd::u32> m_edge_cap;   // slots used (incl. dead)
    crd::containers::Array<crd::u32> m_edge_nodes; // fixed-capacity slabs (appearances bound the fan-in)
    crd::containers::Array<crd::u32> m_appear;
    crd::containers::Array<crd::u64> m_sizes;
    crd::containers::Array<crd::u32> m_ssa_a;
    crd::containers::Array<crd::u32> m_ssa_b;
    crd::u32 m_n_leaves = 0;
    crd::u32 m_n_alive = 0;
    crd::f64 m_flops = 0.0;
};

// Gumbel(0,1) via the shared 53-bit uniform (guarded at u==0, prob 2^-53).
[[nodiscard]] inline crd::f64 gumbel(crd::hesap::stats::PhiloxRng& rng) noexcept
{
    crd::f64 u = rng.next_f64();
    if (u <= 0.0)
    {
        u = 0x1p-53;
    }
    return -std::log(-std::log(u));
}

// The greedy local score (reference-exact): sab/costmod - (sa+sb)*costmod;
// at T>0 the sign-log-compressed score minus T*Gumbel (Boltzmann sampling
// via the Gumbel-min trick).
[[nodiscard]] inline crd::f64 greedy_score(crd::u64 sa, crd::u64 sb, crd::u64 sab, crd::f64 costmod,
                                           crd::f64 temperature, crd::hesap::stats::PhiloxRng& rng) noexcept
{
    const crd::f64 s = static_cast<crd::f64>(sab) / costmod -
                       (static_cast<crd::f64>(sa) + static_cast<crd::f64>(sb)) * costmod;
    if (temperature == 0.0)
    {
        return s;
    }
    crd::f64 base = 0.0;
    if (s > 0.0)
    {
        base = std::log(s);
    }
    else if (s < 0.0)
    {
        base = -std::log(-s);
    }
    return base - temperature * gumbel(rng);
}

} // namespace hyperdetail

// Options for one greedy descent / the random-greedy trial family.
struct HyperGreedyOptions
{
    crd::f64 costmod = 1.0;
    crd::f64 temperature = 0.0;
    crd::u32 max_neighbors = 16;                 // batch-index guard (reference default)
    crd::f64 costmod_range[2] = {0.1, 4.0};      // per-trial uniform draw
    crd::f64 temperature_range[2] = {1e-3, 1.0}; // per-trial log-uniform draw
};

// One T-configurable greedy descent over a (cloned) net. Contracts everything
// (heap over shared-edge pairs, then remaining-by-size); appends SSA pairs into
// the net. Returns Ok, or AllocFailed. flops_limit (0 = off) aborts the trial
// early once its running flops exceed the incumbent (returns NotFound).
[[nodiscard]] inline HyperStatus hyper_greedy(hyperdetail::HyperNet& net, const HyperGreedyOptions& opts,
                                              crd::hesap::stats::PhiloxRng& rng, crd::f64 flops_limit) noexcept;

// ---------------------------------------------------------------------------
// implementation
// ---------------------------------------------------------------------------
namespace hyperdetail
{

inline HyperStatus HyperNet::build(crd::containers::ConstSpan<crd::containers::ConstSpan<crd::u32>> ids,
                                   crd::containers::ConstSpan<crd::u32> out_ids,
                                   crd::containers::ConstSpan<crd::u64> sizes) noexcept
{
    if (ids.size() == 0U)
    {
        return HyperStatus::BadInput;
    }
    const crd::u32 nix = static_cast<crd::u32>(sizes.size());
    if (!m_sizes.try_reserve(nix) || !m_appear.try_reserve(nix))
    {
        return HyperStatus::AllocFailed;
    }
    for (crd::u64 s : sizes)
    {
        if (s == 0U || !m_sizes.try_push_back(s))
        {
            return s == 0U ? HyperStatus::BadInput : HyperStatus::AllocFailed;
        }
    }
    for (crd::u32 k = 0; k < nix; ++k)
    {
        if (!m_appear.try_push_back(0U))
        {
            return HyperStatus::AllocFailed;
        }
    }
    // appearance counts (size-1 indices skipped everywhere, reference rule)
    for (const auto& term : ids)
    {
        for (crd::u32 ix : term)
        {
            if (ix >= nix)
            {
                return HyperStatus::BadInput;
            }
            if (m_sizes[ix] > 1U)
            {
                ++m_appear[ix];
            }
        }
    }
    for (crd::u32 ix : out_ids)
    {
        if (ix >= nix)
        {
            return HyperStatus::BadInput;
        }
        if (m_sizes[ix] > 1U)
        {
            ++m_appear[ix];
        }
    }
    // edge slabs: capacity 2*appear+4 (push bound; compaction reclaims dead)
    crd::u32 pool = 0;
    for (crd::u32 ix = 0; ix < nix; ++ix)
    {
        const crd::u32 cap = 2U * m_appear[ix] + 4U;
        if (!m_edge_off.try_push_back(pool) || !m_edge_len.try_push_back(0U) || !m_edge_cap.try_push_back(0U))
        {
            return HyperStatus::AllocFailed;
        }
        pool += cap;
    }
    if (!m_edge_nodes.try_reserve(pool))
    {
        return HyperStatus::AllocFailed;
    }
    m_edge_nodes.resize(pool, kDead);
    // nodes: sorted unique legs per term
    HyperLeg tmp[64];
    for (const auto& term : ids)
    {
        if (term.size() > 64U)
        {
            return HyperStatus::BadInput;
        }
        crd::u32 nl = 0;
        for (crd::u32 ix : term)
        {
            if (m_sizes[ix] <= 1U)
            {
                continue;
            }
            bool found = false;
            for (crd::u32 k = 0; k < nl; ++k)
            {
                if (tmp[k].ix == ix)
                {
                    ++tmp[k].count;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                tmp[nl].ix = ix;
                tmp[nl].count = 1;
                ++nl;
            }
        }
        // insertion sort by ix (nl <= kMaxRank-ish, tiny)
        for (crd::u32 a = 1; a < nl; ++a)
        {
            const HyperLeg v = tmp[a];
            crd::u32 b = a;
            while (b > 0U && tmp[b - 1].ix > v.ix)
            {
                tmp[b] = tmp[b - 1];
                --b;
            }
            tmp[b] = v;
        }
        const crd::u32 off = static_cast<crd::u32>(m_leg_pool.size());
        for (crd::u32 k = 0; k < nl; ++k)
        {
            if (!m_leg_pool.try_push_back(tmp[k]))
            {
                return HyperStatus::AllocFailed;
            }
        }
        const crd::u32 node = add_node(off, nl);
        if (node == kDead)
        {
            return HyperStatus::AllocFailed;
        }
        for (crd::u32 k = 0; k < nl; ++k)
        {
            if (!edge_push(tmp[k].ix, node))
            {
                return HyperStatus::AllocFailed;
            }
        }
    }
    m_n_leaves = static_cast<crd::u32>(ids.size());
    m_n_alive = m_n_leaves;
    return HyperStatus::Ok;
}

inline crd::u64 HyperNet::flops_of(crd::containers::ConstSpan<HyperLeg> a,
                                   crd::containers::ConstSpan<HyperLeg> b) const noexcept
{
    crd::u64 s = 1;
    crd::u32 i = 0;
    crd::u32 j = 0;
    while (i < a.size() && j < b.size())
    {
        const crd::u32 ia = a[i].ix;
        const crd::u32 ib = b[j].ix;
        if (ia < ib)
        {
            s = sat_mul(s, m_sizes[ia]);
            ++i;
        }
        else if (ib < ia)
        {
            s = sat_mul(s, m_sizes[ib]);
            ++j;
        }
        else
        {
            s = sat_mul(s, m_sizes[ia]);
            ++i;
            ++j;
        }
    }
    for (; i < a.size(); ++i)
    {
        s = sat_mul(s, m_sizes[a[i].ix]);
    }
    for (; j < b.size(); ++j)
    {
        s = sat_mul(s, m_sizes[b[j].ix]);
    }
    return s;
}

inline crd::u32 HyperNet::merge_legs(crd::containers::ConstSpan<HyperLeg> a, crd::containers::ConstSpan<HyperLeg> b,
                                     crd::u32& out_off) noexcept
{
    out_off = static_cast<crd::u32>(m_leg_pool.size());
    crd::u32 n = 0;
    crd::u32 i = 0;
    crd::u32 j = 0;
    const auto push = [&](HyperLeg l) noexcept -> bool
    {
        if (!m_leg_pool.try_push_back(l))
        {
            return false;
        }
        ++n;
        return true;
    };
    while (i < a.size() && j < b.size())
    {
        if (a[i].ix < b[j].ix)
        {
            if (!push(a[i++]))
            {
                return kDead;
            }
        }
        else if (b[j].ix < a[i].ix)
        {
            if (!push(b[j++]))
            {
                return kDead;
            }
        }
        else
        {
            const crd::u32 c = a[i].count + b[j].count;
            if (c < m_appear[a[i].ix])
            {
                if (!push({a[i].ix, c}))
                {
                    return kDead;
                }
            }
            ++i;
            ++j;
        }
    }
    for (; i < a.size(); ++i)
    {
        if (!push(a[i]))
        {
            return kDead;
        }
    }
    for (; j < b.size(); ++j)
    {
        if (!push(b[j]))
        {
            return kDead;
        }
    }
    return n;
}

inline crd::u32 HyperNet::add_node(crd::u32 off, crd::u32 len) noexcept
{
    const crd::u32 id = static_cast<crd::u32>(m_node_off.size());
    if (!m_node_off.try_push_back(off) || !m_node_len.try_push_back(len) || !m_node_alive.try_push_back(1U))
    {
        return kDead;
    }
    ++m_n_alive;
    return id;
}

inline void HyperNet::kill_node(crd::u32 i) noexcept
{
    CRD_ASSERT(m_node_alive[i] != 0U);
    m_node_alive[i] = 0U;
    --m_n_alive;
    for (const HyperLeg& l : legs(i))
    {
        crd::u32* ns = m_edge_nodes.data() + m_edge_off[l.ix];
        for (crd::u32 k = 0; k < m_edge_cap[l.ix]; ++k)
        {
            if (ns[k] == i)
            {
                ns[k] = kDead;
                --m_edge_len[l.ix];
                break;
            }
        }
    }
}

inline bool HyperNet::edge_push(crd::u32 ix, crd::u32 node) noexcept
{
    const crd::u32 cap_total = 2U * m_appear[ix] + 4U;
    crd::u32* ns = m_edge_nodes.data() + m_edge_off[ix];
    if (m_edge_cap[ix] == cap_total)
    {
        // slab full: compact dead slots (alive <= appear <= cap/2 ⇒ always frees)
        crd::u32 w = 0;
        for (crd::u32 k = 0; k < cap_total; ++k)
        {
            if (ns[k] != kDead)
            {
                ns[w++] = ns[k];
            }
        }
        for (crd::u32 k = w; k < cap_total; ++k)
        {
            ns[k] = kDead;
        }
        m_edge_cap[ix] = w;
    }
    CRD_ASSERT(m_edge_cap[ix] < cap_total);
    ns[m_edge_cap[ix]] = node;
    ++m_edge_cap[ix];
    ++m_edge_len[ix];
    return true;
}

inline crd::u32 HyperNet::contract(crd::u32 i, crd::u32 j) noexcept
{
    // Reserve BEFORE taking the leg spans: merge_legs appends to the same pool
    // the spans point into — growth mid-merge would read freed memory (caught
    // by the tree-stats exact-value gate, 2026-07-05).
    if (!m_leg_pool.try_reserve(m_leg_pool.size() + m_node_len[i] + m_node_len[j]))
    {
        return kDead;
    }
    const crd::u64 fl = flops_of(legs(i), legs(j));
    crd::u32 off = 0;
    const crd::u32 len = merge_legs(legs(i), legs(j), off);
    if (len == kDead)
    {
        return kDead;
    }
    kill_node(i);
    kill_node(j);
    m_flops += static_cast<crd::f64>(fl);
    const crd::u32 k = add_node(off, len);
    if (k == kDead)
    {
        return kDead;
    }
    for (const HyperLeg& l : legs(k))
    {
        if (!edge_push(l.ix, k))
        {
            return kDead;
        }
    }
    if (!m_ssa_a.try_push_back(i) || !m_ssa_b.try_push_back(j))
    {
        return kDead;
    }
    return k;
}

inline HyperStatus HyperNet::clone_from(const HyperNet& src) noexcept
{
    const auto copy_u32 = [](crd::containers::Array<crd::u32>& dst,
                             const crd::containers::Array<crd::u32>& s) noexcept -> bool
    {
        if (!dst.try_reserve(s.size()))
        {
            return false;
        }
        dst.resize(s.size());
        for (crd::usize k = 0; k < s.size(); ++k)
        {
            dst[k] = s[k];
        }
        return true;
    };
    if (!m_leg_pool.try_reserve(src.m_leg_pool.size()) || !m_node_alive.try_reserve(src.m_node_alive.size()) ||
        !m_edge_nodes.try_reserve(src.m_edge_nodes.size()) || !m_sizes.try_reserve(src.m_sizes.size()))
    {
        return HyperStatus::AllocFailed;
    }
    m_leg_pool.resize(src.m_leg_pool.size());
    m_node_alive.resize(src.m_node_alive.size());
    m_edge_nodes.resize(src.m_edge_nodes.size());
    m_sizes.resize(src.m_sizes.size());
    for (crd::usize k = 0; k < src.m_leg_pool.size(); ++k)
    {
        m_leg_pool[k] = src.m_leg_pool[k];
    }
    for (crd::usize k = 0; k < src.m_node_alive.size(); ++k)
    {
        m_node_alive[k] = src.m_node_alive[k];
    }
    for (crd::usize k = 0; k < src.m_edge_nodes.size(); ++k)
    {
        m_edge_nodes[k] = src.m_edge_nodes[k];
    }
    for (crd::usize k = 0; k < src.m_sizes.size(); ++k)
    {
        m_sizes[k] = src.m_sizes[k];
    }
    if (!copy_u32(m_node_off, src.m_node_off) || !copy_u32(m_node_len, src.m_node_len) ||
        !copy_u32(m_edge_off, src.m_edge_off) || !copy_u32(m_edge_len, src.m_edge_len) ||
        !copy_u32(m_edge_cap, src.m_edge_cap) || !copy_u32(m_appear, src.m_appear) ||
        !copy_u32(m_ssa_a, src.m_ssa_a) || !copy_u32(m_ssa_b, src.m_ssa_b))
    {
        return HyperStatus::AllocFailed;
    }
    m_n_leaves = src.m_n_leaves;
    m_n_alive = src.m_n_alive;
    m_flops = src.m_flops;
    return HyperStatus::Ok;
}

} // namespace hyperdetail

inline HyperStatus hyper_greedy(hyperdetail::HyperNet& net, const HyperGreedyOptions& opts,
                                crd::hesap::stats::PhiloxRng& rng, crd::f64 flops_limit) noexcept
{
    using hyperdetail::HyperNet;
    struct Entry
    {
        crd::f64 sc;
        crd::u32 ctr;
    };
    struct Cand
    {
        crd::u32 i;
        crd::u32 j;
        crd::u64 ksize;
    };
    // node sizes indexed by ssa id (grown as nodes appear); all scratch borrows
    // the net's allocator (never default_allocator() — the house rule)
    crd::containers::Array<crd::u64> node_sizes(net.allocator());
    crd::containers::Array<Entry> heap(net.allocator());
    crd::containers::Array<Cand> cands(net.allocator());
    const crd::u32 n0 = net.ssa_next();
    if (!node_sizes.try_reserve(2U * n0 + 2U))
    {
        return HyperStatus::AllocFailed;
    }
    node_sizes.resize(n0);
    for (crd::u32 i = 0; i < n0; ++i)
    {
        node_sizes[i] = net.alive(i) ? net.size_of(net.legs(i)) : 0U;
    }
    const auto heap_less = [](const Entry& a, const Entry& b) noexcept
    { return a.sc < b.sc || (a.sc == b.sc && a.ctr < b.ctr); };
    const auto heap_push = [&](Entry e) noexcept -> bool
    {
        if (!heap.try_push_back(e))
        {
            return false;
        }
        crd::usize c = heap.size() - 1U;
        while (c > 0U)
        {
            const crd::usize p = (c - 1U) / 2U;
            if (!heap_less(heap[c], heap[p]))
            {
                break;
            }
            const Entry t = heap[p];
            heap[p] = heap[c];
            heap[c] = t;
            c = p;
        }
        return true;
    };
    const auto heap_pop = [&]() noexcept -> Entry
    {
        const Entry top = heap[0];
        heap[0] = heap[heap.size() - 1U];
        heap.pop_back();
        crd::usize p = 0;
        while (true)
        {
            const crd::usize l = 2U * p + 1U;
            const crd::usize r = l + 1U;
            crd::usize m = p;
            if (l < heap.size() && heap_less(heap[l], heap[m]))
            {
                m = l;
            }
            if (r < heap.size() && heap_less(heap[r], heap[m]))
            {
                m = r;
            }
            if (m == p)
            {
                break;
            }
            const Entry t = heap[p];
            heap[p] = heap[m];
            heap[m] = t;
            p = m;
        }
        return top;
    };
    // Candidates store only (i, j, ksize): merged legs are recomputed at
    // contraction time instead of stored — pool spans don't survive pool
    // growth, and the 3-word table is cheaper than the pool bookkeeping.
    const auto score_pair = [&](crd::u32 i, crd::u32 j, crd::u64& ksize_out) noexcept -> crd::f64
    {
        // merged size without materializing: walk both leg lists
        const auto a = net.legs(i);
        const auto b = net.legs(j);
        crd::u64 s = 1;
        crd::u32 x = 0;
        crd::u32 y = 0;
        while (x < a.size() && y < b.size())
        {
            if (a[x].ix < b[y].ix)
            {
                s = hyperdetail::sat_mul(s, net.index_size(a[x].ix));
                ++x;
            }
            else if (b[y].ix < a[x].ix)
            {
                s = hyperdetail::sat_mul(s, net.index_size(b[y].ix));
                ++y;
            }
            else
            {
                if (a[x].count + b[y].count < net.appearances(a[x].ix))
                {
                    s = hyperdetail::sat_mul(s, net.index_size(a[x].ix));
                }
                ++x;
                ++y;
            }
        }
        for (; x < a.size(); ++x)
        {
            s = hyperdetail::sat_mul(s, net.index_size(a[x].ix));
        }
        for (; y < b.size(); ++y)
        {
            s = hyperdetail::sat_mul(s, net.index_size(b[y].ix));
        }
        ksize_out = s;
        return hyperdetail::greedy_score(node_sizes[i], node_sizes[j], s, opts.costmod, opts.temperature, rng);
    };
    // seed: for every edge within the batch guard, all alive pairs in slab order
    crd::u32 ctr = 0;
    for (crd::u32 ix = 0; ix < net.n_indices(); ++ix)
    {
        crd::u32 members[64];
        crd::u32 nm = 0;
        net.for_edge_members(ix, opts.max_neighbors, [&](crd::u32 node) noexcept
        {
            if (nm < 64U)
            {
                members[nm++] = node;
            }
        });
        for (crd::u32 a = 0; a < nm; ++a)
        {
            for (crd::u32 b = a + 1U; b < nm; ++b)
            {
                crd::u64 ks = 0;
                const crd::f64 sc = score_pair(members[a], members[b], ks);
                if (!cands.try_push_back({members[a], members[b], ks}) || !heap_push({sc, ctr}))
                {
                    return HyperStatus::AllocFailed;
                }
                ++ctr;
            }
        }
    }
    // main loop
    while (heap.size() > 0U)
    {
        const Entry top = heap_pop();
        const Cand c = cands[top.ctr];
        if (!net.alive(c.i) || !net.alive(c.j))
        {
            continue; // stale
        }
        const crd::u32 k = net.contract(c.i, c.j);
        if (k == HyperNet::kDead)
        {
            return HyperStatus::AllocFailed;
        }
        if (flops_limit > 0.0 && net.total_flops() >= flops_limit)
        {
            return HyperStatus::NotFound; // incumbent abort (reference semantics)
        }
        if (!node_sizes.try_reserve(k + 1U))
        {
            return HyperStatus::AllocFailed;
        }
        node_sizes.resize(k + 1U);
        node_sizes[k] = c.ksize;
        net.for_neighbors_limit(k, opts.max_neighbors, [&](crd::u32 l) noexcept
        {
            crd::u64 ms = 0;
            const crd::f64 sc = score_pair(k, l, ms);
            if (cands.try_push_back({k, l, ms}) && heap_push({sc, ctr}))
            {
                ++ctr;
            }
        });
        if (heap.size() >= crd::usize{1} << 14U)
        {
            // prune stale entries (both nodes must still be alive)
            crd::usize w = 0;
            for (crd::usize r = 0; r < heap.size(); ++r)
            {
                const Cand& cc = cands[heap[r].ctr];
                if (net.alive(cc.i) && net.alive(cc.j))
                {
                    heap[w++] = heap[r];
                }
            }
            while (heap.size() > w)
            {
                heap.pop_back();
            }
            // heapify (Floyd)
            for (crd::usize p = heap.size() / 2U; p-- > 0U;)
            {
                crd::usize q = p;
                while (true)
                {
                    const crd::usize l = 2U * q + 1U;
                    const crd::usize r2 = l + 1U;
                    crd::usize m = q;
                    if (l < heap.size() && heap_less(heap[l], heap[m]))
                    {
                        m = l;
                    }
                    if (r2 < heap.size() && heap_less(heap[r2], heap[m]))
                    {
                        m = r2;
                    }
                    if (m == q)
                    {
                        break;
                    }
                    const Entry t = heap[q];
                    heap[q] = heap[m];
                    heap[m] = t;
                    q = m;
                }
            }
        }
    }
    // leftovers (disconnected / batch-guarded): smallest-size-first
    while (net.n_alive() > 1U)
    {
        crd::u32 best_i = HyperNet::kDead;
        crd::u32 best_j = HyperNet::kDead;
        crd::u64 s1 = hyperdetail::kSat;
        crd::u64 s2 = hyperdetail::kSat;
        for (crd::u32 i = 0; i < net.ssa_next(); ++i)
        {
            if (!net.alive(i))
            {
                continue;
            }
            const crd::u64 s = net.size_of(net.legs(i));
            if (s < s1 || (s == s1 && best_i == HyperNet::kDead))
            {
                s2 = s1;
                best_j = best_i;
                s1 = s;
                best_i = i;
            }
            else if (s < s2)
            {
                s2 = s;
                best_j = i;
            }
        }
        if (best_j == HyperNet::kDead)
        {
            break;
        }
        if (net.contract(best_i, best_j) == HyperNet::kDead)
        {
            return HyperStatus::AllocFailed;
        }
    }
    return HyperStatus::Ok;
}

// ---------------------------------------------------------------------------
// Objectives (cotengra scoring.py, exact log2 forms) + the contraction TREE
// tier: HyperTree replays a contracted net's SSA records into a mutable binary
// tree; subtree_reconfigure re-solves <=subtree_size-leaf subtrees with an
// exact subset DP (deterministic select-max candidates, BFS frontiers).
// ---------------------------------------------------------------------------
enum class HyperObjective : crd::u8
{
    Flops,
    Size,
    Combo, // log2(flops + factor*write), factor default 64
};

struct HyperTreeStats
{
    crd::f64 flops = 0.0;
    crd::f64 write = 0.0;
    crd::u64 size = 1;
};

[[nodiscard]] inline crd::f64 hyper_score_tree(HyperObjective obj, const HyperTreeStats& st,
                                               crd::f64 factor = 64.0) noexcept
{
    const crd::f64 fl = st.flops > 1.0 ? st.flops : 1.0;
    const crd::f64 wr = st.write > 1.0 ? st.write : 1.0;
    const crd::f64 sz = static_cast<crd::f64>(st.size > 1U ? st.size : 1U);
    switch (obj)
    {
    case HyperObjective::Flops:
        return std::log2(fl) + 1e-3 * std::log2(wr) + 1e-3 * std::log2(sz);
    case HyperObjective::Size:
        return std::log2(sz) + 1e-3 * std::log2(fl) + 1e-3 * std::log2(wr);
    case HyperObjective::Combo:
    default:
        return std::log2(fl + factor * wr);
    }
}

class HyperTree
{
public:
    explicit HyperTree(crd::memory::IAllocator* alloc) noexcept
        : m_pool(alloc), m_off(alloc), m_len(alloc), m_child_a(alloc), m_child_b(alloc), m_scratch(alloc),
          m_sizes(alloc), m_appear(alloc), m_alloc(alloc)
    {
    }

    static constexpr crd::u32 kLeaf = hyperdetail::HyperNet::kDead;

    // Build from a fully-contracted net (its SSA records + per-node legs).
    // The tree COPIES the index metadata (sizes/appearances): it must never
    // borrow the net's lifetime — a tree returned/kept beyond the net's scope
    // dangled through exactly that borrow (UAF root-caused 2026-07-05:
    // gcc -O3 silent, MSVC-debug SEGV, ASan assert — the borrowed-lifetime
    // member is the same scar class as allocator-outlives-borrowers).
    [[nodiscard]] HyperStatus build(const hyperdetail::HyperNet& net) noexcept
    {
        const crd::u32 nix = net.n_indices();
        m_sizes.resize(0);
        m_appear.resize(0);
        if (!m_sizes.try_reserve(nix) || !m_appear.try_reserve(nix))
        {
            return HyperStatus::AllocFailed;
        }
        for (crd::u32 ix = 0; ix < nix; ++ix)
        {
            if (!m_sizes.try_push_back(net.index_size(ix)) || !m_appear.try_push_back(net.appearances(ix)))
            {
                return HyperStatus::AllocFailed;
            }
        }
        const crd::u32 n = net.ssa_next();
        if (net.n_alive() != 1U || n < 1U)
        {
            return HyperStatus::BadInput;
        }
        if (!m_off.try_reserve(2U * n) || !m_len.try_reserve(2U * n) || !m_child_a.try_reserve(2U * n) ||
            !m_child_b.try_reserve(2U * n))
        {
            return HyperStatus::AllocFailed;
        }
        m_off.resize(n);
        m_len.resize(n);
        m_child_a.resize(n, kLeaf);
        m_child_b.resize(n, kLeaf);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const auto legs = net.legs(i);
            m_off[i] = static_cast<crd::u32>(m_pool.size());
            m_len[i] = static_cast<crd::u32>(legs.size());
            for (const HyperLeg& l : legs)
            {
                if (!m_pool.try_push_back(l))
                {
                    return HyperStatus::AllocFailed;
                }
            }
        }
        const auto sa = net.ssa_a();
        const auto sb = net.ssa_b();
        m_n_leaves = net.n_leaves();
        for (crd::u32 k = 0; k < sa.size(); ++k)
        {
            const crd::u32 p = m_n_leaves + k;
            m_child_a[p] = sa[k];
            m_child_b[p] = sb[k];
        }
        m_root = n - 1U;
        return HyperStatus::Ok;
    }

    [[nodiscard]] crd::u32 root() const noexcept { return m_root; }
    [[nodiscard]] crd::u32 n_nodes() const noexcept { return static_cast<crd::u32>(m_off.size()); }
    [[nodiscard]] bool is_internal(crd::u32 p) const noexcept { return m_child_a[p] != kLeaf; }
    [[nodiscard]] crd::u32 child_a(crd::u32 p) const noexcept { return m_child_a[p]; }
    [[nodiscard]] crd::u32 child_b(crd::u32 p) const noexcept { return m_child_b[p]; }

    [[nodiscard]] crd::containers::ConstSpan<HyperLeg> legs(crd::u32 i) const noexcept
    {
        return {m_pool.data() + m_off[i], m_len[i]};
    }

    // size / flops over OWNED metadata (mirrors HyperNet's definitions)
    [[nodiscard]] crd::u64 size_of(crd::containers::ConstSpan<HyperLeg> ls) const noexcept
    {
        crd::u64 s = 1;
        for (const HyperLeg& l : ls)
        {
            s = hyperdetail::sat_mul(s, m_sizes[l.ix]);
        }
        return s;
    }

    [[nodiscard]] crd::u64 flops_of(crd::containers::ConstSpan<HyperLeg> a,
                                    crd::containers::ConstSpan<HyperLeg> b) const noexcept
    {
        crd::u64 s = 1;
        crd::u32 i = 0;
        crd::u32 j = 0;
        while (i < a.size() && j < b.size())
        {
            const crd::u32 ia = a[i].ix;
            const crd::u32 ib = b[j].ix;
            s = hyperdetail::sat_mul(s, m_sizes[ia < ib ? ia : ib]);
            i += static_cast<crd::u32>(ia <= ib);
            j += static_cast<crd::u32>(ib <= ia);
        }
        for (; i < a.size(); ++i)
        {
            s = hyperdetail::sat_mul(s, m_sizes[a[i].ix]);
        }
        for (; j < b.size(); ++j)
        {
            s = hyperdetail::sat_mul(s, m_sizes[b[j].ix]);
        }
        return s;
    }

    [[nodiscard]] crd::u64 index_size(crd::u32 ix) const noexcept { return m_sizes[ix]; }
    [[nodiscard]] crd::u32 appearances(crd::u32 ix) const noexcept { return m_appear[ix]; }

    [[nodiscard]] crd::u64 node_size(crd::u32 p) const noexcept { return size_of(legs(p)); }
    [[nodiscard]] crd::u64 node_flops(crd::u32 p) const noexcept
    {
        return flops_of(legs(m_child_a[p]), legs(m_child_b[p]));
    }

    [[nodiscard]] HyperTreeStats stats() const noexcept
    {
        HyperTreeStats st;
        for (crd::u32 p = 0; p < n_nodes(); ++p)
        {
            if (!is_internal(p) || !alive_node(p))
            {
                continue;
            }
            st.flops += static_cast<crd::f64>(node_flops(p));
            const crd::u64 s = node_size(p);
            st.write += static_cast<crd::f64>(s);
            if (s > st.size)
            {
                st.size = s;
            }
        }
        return st;
    }

    // Deterministic subtree reconfiguration: candidates = internal nodes by
    // descending contraction flops (id-ascending ties); per candidate a BFS
    // frontier of <= subtree_size nodes is re-solved with an exact subset DP
    // and spliced back when strictly cheaper. maxiter bounds DP ATTEMPTS (the
    // reference memoizes repeated frontiers instead — a perf cache, not
    // semantics; the attempt bound keeps WCET explicit).
    HyperStatus reconfigure(crd::u32 subtree_size, crd::u32 maxiter, HyperObjective obj,
                            crd::f64 factor = 64.0) noexcept;

    // treesa simulated annealing (Kalachev-class tree surgery; reference:
    // path_simulated_annealing.py, python-verified). Geometric temperature
    // ladder tstart→tfinal over tsteps; per temperature `numiter` BFS sweeps;
    // at each internal parent one of the 4 associativity rotations is proposed
    // and accepted iff dE <= 0 or log(u) < -dE/T (dE in the local log2 score of
    // the two affected contractions). Deterministic given (seed, stream).
    HyperStatus anneal(crd::f64 tstart, crd::f64 tfinal, crd::u32 tsteps, crd::u32 numiter, HyperObjective obj,
                       crd::f64 factor, crd::u64 seed, crd::u64 stream) noexcept;

private:
    friend struct HyperTreeTestPeek;

    // a node is dead when it was orphaned by a splice (parent tracking is
    // implicit: dead nodes have both children set to themselves)
    [[nodiscard]] bool alive_node(crd::u32 p) const noexcept { return m_child_a[p] != p; }
    void kill(crd::u32 p) noexcept
    {
        m_child_a[p] = p;
        m_child_b[p] = p;
    }

    // append merged legs of (a, b) to the pool (scratch-materialized first —
    // pool growth invalidates input spans); returns HyperStatus via out params
    [[nodiscard]] bool merge_into_pool(crd::u32 a, crd::u32 b, crd::u32& off, crd::u32& len) noexcept;
    // merge into m_scratch only (no pool commit — SA proposals are usually
    // rejected; committing every proposal would grow the arena unboundedly)
    [[nodiscard]] bool merge_into_scratch(crd::u32 a, crd::u32 b) noexcept;
    // commit the current scratch to the pool; returns its (off, len)
    [[nodiscard]] bool commit_scratch(crd::u32& off, crd::u32& len) noexcept;

    crd::containers::Array<HyperLeg> m_pool;
    crd::containers::Array<crd::u32> m_off;
    crd::containers::Array<crd::u32> m_len;
    crd::containers::Array<crd::u32> m_child_a;
    crd::containers::Array<crd::u32> m_child_b;
    crd::containers::Array<HyperLeg> m_scratch;
    // OWNED index metadata (copied at build — never a borrowed net lifetime)
    crd::containers::Array<crd::u64> m_sizes;
    crd::containers::Array<crd::u32> m_appear;
    crd::memory::IAllocator* m_alloc;
    crd::u32 m_n_leaves = 0;
    crd::u32 m_root = 0;
};

inline bool HyperTree::commit_scratch(crd::u32& off, crd::u32& len) noexcept
{
    off = static_cast<crd::u32>(m_pool.size());
    len = static_cast<crd::u32>(m_scratch.size());
    for (const HyperLeg& l : m_scratch)
    {
        if (!m_pool.try_push_back(l))
        {
            return false;
        }
    }
    return true;
}

inline bool HyperTree::merge_into_scratch(crd::u32 a, crd::u32 b) noexcept
{
    m_scratch.resize(0);
    const auto la = legs(a);
    const auto lb = legs(b);
    crd::u32 i = 0;
    crd::u32 j = 0;
    const auto push = [&](HyperLeg l) noexcept -> bool { return m_scratch.try_push_back(l); };
    while (i < la.size() && j < lb.size())
    {
        if (la[i].ix < lb[j].ix)
        {
            if (!push(la[i++]))
            {
                return false;
            }
        }
        else if (lb[j].ix < la[i].ix)
        {
            if (!push(lb[j++]))
            {
                return false;
            }
        }
        else
        {
            const crd::u32 c = la[i].count + lb[j].count;
            if (c < appearances(la[i].ix))
            {
                if (!push({la[i].ix, c}))
                {
                    return false;
                }
            }
            ++i;
            ++j;
        }
    }
    for (; i < la.size(); ++i)
    {
        if (!push(la[i]))
        {
            return false;
        }
    }
    for (; j < lb.size(); ++j)
    {
        if (!push(lb[j]))
        {
            return false;
        }
    }
    return true;
}

inline bool HyperTree::merge_into_pool(crd::u32 a, crd::u32 b, crd::u32& off, crd::u32& len) noexcept
{
    return merge_into_scratch(a, b) && commit_scratch(off, len);
}

inline HyperStatus HyperTree::reconfigure(crd::u32 subtree_size, crd::u32 maxiter, HyperObjective obj,
                                          crd::f64 factor) noexcept
{
    if (subtree_size < 3U || subtree_size > 10U)
    {
        return HyperStatus::BadInput;
    }
    const crd::f64 cf = obj == HyperObjective::Combo ? factor : 0.0;
    crd::containers::Array<crd::u32> cands(m_alloc);
    crd::containers::Array<crd::u32> frontier(m_alloc);
    crd::containers::Array<crd::u32> walk(m_alloc);
    // subset-DP tables over <= 2^10 masks
    crd::containers::Array<crd::f64> dp_cost(m_alloc);
    crd::containers::Array<crd::u32> dp_off(m_alloc);
    crd::containers::Array<crd::u32> dp_len(m_alloc);
    crd::containers::Array<crd::u32> dp_split(m_alloc);
    crd::containers::Array<HyperLeg> dp_pool(m_alloc);
    crd::containers::Array<crd::u32> idmap(m_alloc);
    crd::u32 iters = 0;
    bool improved_any = true;
    while (improved_any && iters < maxiter)
    {
        improved_any = false;
        // candidates: alive internal nodes, flops-descending (id-ascending ties)
        cands.resize(0);
        for (crd::u32 p = 0; p < n_nodes(); ++p)
        {
            if (is_internal(p) && alive_node(p))
            {
                if (!cands.try_push_back(p))
                {
                    return HyperStatus::AllocFailed;
                }
            }
        }
        const auto by_flops_desc = [this](crd::u32 x, crd::u32 y) noexcept
        {
            const crd::u64 fx = node_flops(x);
            const crd::u64 fy = node_flops(y);
            return fx > fy || (fx == fy && x < y);
        };
        crd::containers::sort(cands.data(), cands.data() + cands.size(), by_flops_desc);
        for (crd::usize ci = 0; ci < cands.size() && iters < maxiter; ++ci)
        {
            const crd::u32 sub_root = cands[ci];
            if (!alive_node(sub_root) || !is_internal(sub_root))
            {
                continue;
            }
            // BFS frontier: expand the first expandable entry until subtree_size
            frontier.resize(0);
            if (!frontier.try_push_back(sub_root))
            {
                return HyperStatus::AllocFailed;
            }
            while (true)
            {
                crd::u32 grow = kLeaf;
                for (crd::u32 f = 0; f < frontier.size(); ++f)
                {
                    if (is_internal(frontier[f]) && frontier.size() < subtree_size)
                    {
                        grow = f;
                        break;
                    }
                }
                if (grow == kLeaf)
                {
                    break;
                }
                const crd::u32 x = frontier[grow];
                // remove position `grow` (order-preserving shift, reference BFS)
                for (crd::u32 f = grow; f + 1U < frontier.size(); ++f)
                {
                    frontier[f] = frontier[f + 1U];
                }
                frontier.pop_back();
                if (!frontier.try_push_back(m_child_a[x]) || !frontier.try_push_back(m_child_b[x]))
                {
                    return HyperStatus::AllocFailed;
                }
            }
            const crd::u32 nf = static_cast<crd::u32>(frontier.size());
            if (nf < 3U)
            {
                continue;
            }
            ++iters;
            // current cost of the subtree's internal contractions (frontier-stopped)
            crd::f64 cur = 0.0;
            walk.resize(0);
            if (!walk.try_push_back(sub_root))
            {
                return HyperStatus::AllocFailed;
            }
            while (walk.size() > 0U)
            {
                const crd::u32 x = walk[walk.size() - 1U];
                walk.pop_back();
                bool on_frontier = false;
                for (crd::u32 f = 0; f < nf; ++f)
                {
                    if (frontier[f] == x)
                    {
                        on_frontier = true;
                        break;
                    }
                }
                if (on_frontier || !is_internal(x))
                {
                    continue;
                }
                cur += static_cast<crd::f64>(node_flops(x)) + cf * static_cast<crd::f64>(node_size(x));
                if (!walk.try_push_back(m_child_a[x]) || !walk.try_push_back(m_child_b[x]))
                {
                    return HyperStatus::AllocFailed;
                }
            }
            // exact subset DP over the frontier
            const crd::u32 full = (1U << nf) - 1U;
            dp_cost.resize(0);
            dp_off.resize(0);
            dp_len.resize(0);
            dp_split.resize(0);
            dp_pool.resize(0);
            if (!dp_cost.try_reserve(full + 1U) || !dp_off.try_reserve(full + 1U) || !dp_len.try_reserve(full + 1U) ||
                !dp_split.try_reserve(full + 1U))
            {
                return HyperStatus::AllocFailed;
            }
            dp_cost.resize(full + 1U, -1.0);
            dp_off.resize(full + 1U, 0U);
            dp_len.resize(full + 1U, 0U);
            dp_split.resize(full + 1U, 0U);
            for (crd::u32 k = 0; k < nf; ++k)
            {
                const crd::u32 m = 1U << k;
                dp_cost[m] = 0.0;
                const auto l = legs(frontier[k]);
                dp_off[m] = static_cast<crd::u32>(dp_pool.size());
                dp_len[m] = static_cast<crd::u32>(l.size());
                for (const HyperLeg& lg : l)
                {
                    if (!dp_pool.try_push_back(lg))
                    {
                        return HyperStatus::AllocFailed;
                    }
                }
            }
            for (crd::u32 m = 1; m <= full; ++m)
            {
                if (std::popcount(m) < 2)
                {
                    continue;
                }
                for (crd::u32 s = (m - 1U) & m; s != 0U; s = (s - 1U) & m)
                {
                    const crd::u32 o = m ^ s;
                    if (s < o)
                    {
                        continue; // canonical split only
                    }
                    if (dp_cost[s] < 0.0 || dp_cost[o] < 0.0)
                    {
                        continue;
                    }
                    const crd::containers::ConstSpan<HyperLeg> ls{dp_pool.data() + dp_off[s], dp_len[s]};
                    const crd::containers::ConstSpan<HyperLeg> lo{dp_pool.data() + dp_off[o], dp_len[o]};
                    const crd::u64 fl = flops_of(ls, lo);
                    crd::f64 step = static_cast<crd::f64>(fl);
                    crd::u32 moff = 0;
                    crd::u32 mlen = 0;
                    if (dp_cost[m] < 0.0 || dp_cost[s] + dp_cost[o] + step < dp_cost[m] || cf > 0.0)
                    {
                        // materialize merged legs (needed for both size term and children)
                        m_scratch.resize(0);
                        crd::u32 x2 = 0;
                        crd::u32 y2 = 0;
                        bool ok = true;
                        while (x2 < ls.size() && y2 < lo.size())
                        {
                            if (ls[x2].ix < lo[y2].ix)
                            {
                                ok = ok && m_scratch.try_push_back(ls[x2++]);
                            }
                            else if (lo[y2].ix < ls[x2].ix)
                            {
                                ok = ok && m_scratch.try_push_back(lo[y2++]);
                            }
                            else
                            {
                                const crd::u32 c = ls[x2].count + lo[y2].count;
                                if (c < appearances(ls[x2].ix))
                                {
                                    ok = ok && m_scratch.try_push_back({ls[x2].ix, c});
                                }
                                ++x2;
                                ++y2;
                            }
                        }
                        for (; x2 < ls.size(); ++x2)
                        {
                            ok = ok && m_scratch.try_push_back(ls[x2]);
                        }
                        for (; y2 < lo.size(); ++y2)
                        {
                            ok = ok && m_scratch.try_push_back(lo[y2]);
                        }
                        if (!ok)
                        {
                            return HyperStatus::AllocFailed;
                        }
                        if (cf > 0.0)
                        {
                            crd::u64 msz = 1;
                            for (const HyperLeg& lg : m_scratch)
                            {
                                msz = hyperdetail::sat_mul(msz, index_size(lg.ix));
                            }
                            step += cf * static_cast<crd::f64>(msz);
                        }
                        const crd::f64 tot = dp_cost[s] + dp_cost[o] + step;
                        if (dp_cost[m] < 0.0 || tot < dp_cost[m])
                        {
                            moff = static_cast<crd::u32>(dp_pool.size());
                            mlen = static_cast<crd::u32>(m_scratch.size());
                            for (const HyperLeg& lg : m_scratch)
                            {
                                if (!dp_pool.try_push_back(lg))
                                {
                                    return HyperStatus::AllocFailed;
                                }
                            }
                            dp_cost[m] = tot;
                            dp_off[m] = moff;
                            dp_len[m] = mlen;
                            dp_split[m] = s;
                        }
                    }
                }
            }
            if (dp_cost[full] < 0.0 || dp_cost[full] >= cur)
            {
                continue; // no strict improvement
            }
            // splice: kill old internals under sub_root (not the frontier, not
            // sub_root itself), then rebuild per DP splits bottom-up
            walk.resize(0);
            if (!walk.try_push_back(sub_root))
            {
                return HyperStatus::AllocFailed;
            }
            while (walk.size() > 0U)
            {
                const crd::u32 x = walk[walk.size() - 1U];
                walk.pop_back();
                bool on_frontier = false;
                for (crd::u32 f = 0; f < nf; ++f)
                {
                    if (frontier[f] == x)
                    {
                        on_frontier = true;
                        break;
                    }
                }
                if (on_frontier || !is_internal(x))
                {
                    continue;
                }
                if (!walk.try_push_back(m_child_a[x]) || !walk.try_push_back(m_child_b[x]))
                {
                    return HyperStatus::AllocFailed;
                }
                if (x != sub_root)
                {
                    kill(x);
                }
            }
            // idmap: mask -> node id (leaf masks pre-filled)
            idmap.resize(0);
            if (!idmap.try_reserve(full + 1U))
            {
                return HyperStatus::AllocFailed;
            }
            idmap.resize(full + 1U, kLeaf);
            for (crd::u32 k = 0; k < nf; ++k)
            {
                idmap[crd::usize{1} << k] = frontier[k];
            }
            // iterative unwind of the winning decomposition (stack of masks;
            // only masks on the winning dp_split path get node ids)
            walk.resize(0);
            if (!walk.try_push_back(full))
            {
                return HyperStatus::AllocFailed;
            }
            // two-phase: first ensure children exist, then emit parent
            while (walk.size() > 0U)
            {
                const crd::u32 m = walk[walk.size() - 1U];
                if (std::popcount(m) == 1 || idmap[m] != kLeaf)
                {
                    walk.pop_back();
                    continue;
                }
                const crd::u32 s = dp_split[m];
                const crd::u32 o = m ^ s;
                const bool s_ready = std::popcount(s) == 1 || idmap[s] != kLeaf;
                const bool o_ready = std::popcount(o) == 1 || idmap[o] != kLeaf;
                if (!s_ready)
                {
                    if (!walk.try_push_back(s))
                    {
                        return HyperStatus::AllocFailed;
                    }
                    continue;
                }
                if (!o_ready)
                {
                    if (!walk.try_push_back(o))
                    {
                        return HyperStatus::AllocFailed;
                    }
                    continue;
                }
                walk.pop_back();
                const crd::u32 ca = idmap[s]; // leaf masks pre-filled
                const crd::u32 cb = idmap[o];
                crd::u32 p = 0;
                if (m == full)
                {
                    p = sub_root; // reuse the sub-root label (legs unchanged)
                }
                else
                {
                    p = n_nodes();
                    crd::u32 off = 0;
                    crd::u32 len = 0;
                    // legs from the DP table (already merged there)
                    off = static_cast<crd::u32>(m_pool.size());
                    len = dp_len[m];
                    for (crd::u32 q = 0; q < len; ++q)
                    {
                        if (!m_pool.try_push_back(dp_pool[dp_off[m] + q]))
                        {
                            return HyperStatus::AllocFailed;
                        }
                    }
                    if (!m_off.try_push_back(off) || !m_len.try_push_back(len) ||
                        !m_child_a.try_push_back(kLeaf) || !m_child_b.try_push_back(kLeaf))
                    {
                        return HyperStatus::AllocFailed;
                    }
                }
                m_child_a[p] = ca;
                m_child_b[p] = cb;
                idmap[m] = p;
            }
            improved_any = true;
        }
    }
    return HyperStatus::Ok;
}

// ---------------------------------------------------------------------------
// SliceFinder (reference: slicer.py, python-verified to matched-tree parity):
// pick indices to slice until max intermediate <= target_size. Incremental
// integer-exact cost structure; candidates scored ln(flop_red + 1e-3*write_red
// + 1) + T*Gumbel (argmax); best-of-max_repeats by (total_flops, nslices,
// size). The returned bound is EXACT: max sliced intermediate <= target_size
// or NotFound (the ADR-0095 WCET pillar — never a best-effort answer).
// ---------------------------------------------------------------------------
struct HyperSliceResult
{
    crd::containers::Array<crd::u32> indices; // sliced index ids
    crd::u64 nslices = 1;                     // product of sliced dims
    crd::f64 sliced_flops = 0.0;              // nslices * per-slice flops
    crd::f64 overhead = 1.0;                  // sliced_flops / original flops
    crd::u64 max_size = 1;                    // post-slice max intermediate
    explicit HyperSliceResult(crd::memory::IAllocator* alloc) : indices(alloc) {}
};

namespace hyperdetail
{

// per-repeat working state over the tree's contractions
class SliceCosts
{
public:
    explicit SliceCosts(crd::memory::IAllocator* alloc) noexcept
        : m_con_size(alloc), m_con_flops(alloc), m_con_ix(alloc), m_con_ix_off(alloc), m_con_ix_len(alloc),
          m_con_leg_mask(alloc), m_flop_red(alloc), m_write_red(alloc), m_ix_alive(alloc)
    {
    }

    [[nodiscard]] HyperStatus init(const HyperTree& tree, const HyperNet& net) noexcept
    {
        m_net = &net;
        m_flops = 0.0;
        m_nslices = 1.0;
        // collect internal contractions
        for (crd::u32 p = 0; p < tree.n_nodes(); ++p)
        {
            if (!tree.is_internal(p) || tree.child_a(p) == p)
            {
                continue;
            }
            const crd::u32 a = tree.child_a(p);
            const crd::u32 b = tree.child_b(p);
            const crd::u64 fl = net.flops_of(tree.legs(a), tree.legs(b));
            const crd::u64 sz = net.size_of(tree.legs(p));
            const crd::u32 ixo = static_cast<crd::u32>(m_con_ix.size());
            // involved = union of children's indices; leg-mask marks output legs
            crd::u32 cnt = 0;
            const auto la = tree.legs(a);
            const auto lb = tree.legs(b);
            const auto lp = tree.legs(p);
            crd::u32 i = 0;
            crd::u32 j = 0;
            const auto emit = [&](crd::u32 ix) noexcept -> bool
            {
                bool in_legs = false;
                for (const HyperLeg& l : lp)
                {
                    if (l.ix == ix)
                    {
                        in_legs = true;
                        break;
                    }
                }
                if (!m_con_ix.try_push_back(ix) || !m_con_leg_mask.try_push_back(in_legs ? 1U : 0U))
                {
                    return false;
                }
                ++cnt;
                return true;
            };
            while (i < la.size() && j < lb.size())
            {
                const crd::u32 xa = la[i].ix;
                const crd::u32 xb = lb[j].ix;
                const crd::u32 ix = xa < xb ? xa : xb;
                if (!emit(ix))
                {
                    return HyperStatus::AllocFailed;
                }
                i += static_cast<crd::u32>(xa <= xb);
                j += static_cast<crd::u32>(xb <= xa);
            }
            for (; i < la.size(); ++i)
            {
                if (!emit(la[i].ix))
                {
                    return HyperStatus::AllocFailed;
                }
            }
            for (; j < lb.size(); ++j)
            {
                if (!emit(lb[j].ix))
                {
                    return HyperStatus::AllocFailed;
                }
            }
            if (!m_con_size.try_push_back(sz) || !m_con_flops.try_push_back(fl) ||
                !m_con_ix_off.try_push_back(ixo) || !m_con_ix_len.try_push_back(cnt))
            {
                return HyperStatus::AllocFailed;
            }
            m_flops += static_cast<crd::f64>(fl);
        }
        m_original_flops = m_flops;
        // per-index alive flags + reductions
        const crd::u32 nix = net.n_indices();
        if (!m_ix_alive.try_reserve(nix) || !m_flop_red.try_reserve(nix) || !m_write_red.try_reserve(nix))
        {
            return HyperStatus::AllocFailed;
        }
        m_ix_alive.resize(nix, 0U);
        m_flop_red.resize(nix, 0U);
        m_write_red.resize(nix, 0U);
        for (crd::usize c = 0; c < m_con_size.size(); ++c)
        {
            accumulate(static_cast<crd::u32>(c), +1, true);
        }
        return HyperStatus::Ok;
    }

    [[nodiscard]] crd::u64 max_size() const noexcept
    {
        crd::u64 m = 1;
        for (crd::u64 s : m_con_size)
        {
            if (s > m)
            {
                m = s;
            }
        }
        return m;
    }

    [[nodiscard]] crd::f64 total_flops() const noexcept { return m_nslices * m_flops; }
    [[nodiscard]] crd::f64 nslices() const noexcept { return m_nslices; }
    [[nodiscard]] crd::f64 original_flops() const noexcept { return m_original_flops; }
    [[nodiscard]] bool ix_alive(crd::u32 ix) const noexcept { return m_ix_alive[ix] != 0U; }
    [[nodiscard]] crd::u64 flop_red(crd::u32 ix) const noexcept { return m_flop_red[ix]; }
    [[nodiscard]] crd::u64 write_red(crd::u32 ix) const noexcept { return m_write_red[ix]; }
    [[nodiscard]] crd::u32 n_indices() const noexcept { return static_cast<crd::u32>(m_ix_alive.size()); }

    // slice ix: integer-exact floor-division updates (reference semantics)
    void remove(crd::u32 ix) noexcept
    {
        const crd::u64 d = m_net->index_size(ix);
        m_nslices *= static_cast<crd::f64>(d);
        for (crd::usize c = 0; c < m_con_size.size(); ++c)
        {
            bool involved = false;
            bool in_legs = false;
            const crd::u32 off = m_con_ix_off[c];
            for (crd::u32 k = 0; k < m_con_ix_len[c]; ++k)
            {
                if (m_con_ix[off + k] == ix)
                {
                    involved = true;
                    in_legs = m_con_leg_mask[off + k] != 0U;
                    break;
                }
            }
            if (!involved)
            {
                continue;
            }
            accumulate(static_cast<crd::u32>(c), -1, false);
            m_flops -= static_cast<crd::f64>(m_con_flops[c]);
            m_con_flops[c] /= d;
            m_flops += static_cast<crd::f64>(m_con_flops[c]);
            if (in_legs)
            {
                m_con_size[c] /= d;
            }
            accumulate(static_cast<crd::u32>(c), +1, false);
        }
        m_ix_alive[ix] = 0U;
        m_flop_red[ix] = 0U;
        m_write_red[ix] = 0U;
    }

private:
    // Add (+1) or subtract (-1) contraction c's contributions to the per-index
    // reduction accumulators. init_mode marks indices alive on first discovery;
    // afterwards sliced (dead) indices never re-accumulate. (The reference
    // applies closed-form deltas; subtract-update-readd is arithmetically
    // identical and planner-scale cheap.)
    void accumulate(crd::u32 c, int sign, bool init_mode) noexcept
    {
        const crd::u64 fl = m_con_flops[c];
        const crd::u64 sz = m_con_size[c];
        const crd::u32 off = m_con_ix_off[c];
        for (crd::u32 k = 0; k < m_con_ix_len[c]; ++k)
        {
            const crd::u32 ox = m_con_ix[off + k];
            if (init_mode)
            {
                m_ix_alive[ox] = 1U;
            }
            if (m_ix_alive[ox] == 0U)
            {
                continue; // sliced — never re-accumulates
            }
            const crd::u64 di = m_net->index_size(ox);
            const crd::u64 fred = fl - fl / di;
            const crd::u64 sred = m_con_leg_mask[off + k] != 0U ? sz - sz / di : 0U;
            if (sign > 0)
            {
                m_flop_red[ox] += fred;
                m_write_red[ox] += sred;
            }
            else
            {
                m_flop_red[ox] -= fred;
                m_write_red[ox] -= sred;
            }
        }
    }

    const HyperNet* m_net = nullptr;
    crd::containers::Array<crd::u64> m_con_size;
    crd::containers::Array<crd::u64> m_con_flops;
    crd::containers::Array<crd::u32> m_con_ix;
    crd::containers::Array<crd::u32> m_con_ix_off;
    crd::containers::Array<crd::u32> m_con_ix_len;
    crd::containers::Array<crd::u8> m_con_leg_mask;
    crd::containers::Array<crd::u64> m_flop_red;
    crd::containers::Array<crd::u64> m_write_red;
    crd::containers::Array<crd::u8> m_ix_alive;
    crd::f64 m_flops = 0.0;
    crd::f64 m_original_flops = 0.0;
    crd::f64 m_nslices = 1.0;
};

} // namespace hyperdetail

// Slice the tree's contraction to fit target_size (elements). Output indices
// are sliceable by default (reference allow_outer=True). Returns Ok with the
// EXACT bound satisfied, or NotFound when no candidate set can reach it.
[[nodiscard]] inline HyperStatus hyper_slice(const HyperTree& tree, const hyperdetail::HyperNet& net,
                                             crd::u64 target_size, crd::u64 seed, crd::u32 max_repeats,
                                             crd::f64 temperature, HyperSliceResult& out) noexcept
{
    using hyperdetail::SliceCosts;
    crd::memory::IAllocator* alloc = net.allocator();
    bool have_best = false;
    crd::f64 best_key0 = 0.0;
    crd::f64 best_key1 = 0.0;
    crd::u64 best_key2 = 0;
    crd::containers::Array<crd::u32> chosen(alloc);
    for (crd::u32 rep = 0; rep < max_repeats; ++rep)
    {
        crd::hesap::stats::PhiloxRng rng(seed, 0x51D0ULL << 16 | rep);
        SliceCosts costs(alloc);
        if (costs.init(tree, net) != HyperStatus::Ok)
        {
            return HyperStatus::AllocFailed;
        }
        chosen.resize(0);
        while (costs.max_size() > target_size)
        {
            // argmax over alive indices: ln(fred + 1e-3*wred + 1) + T*gumbel
            bool found = false;
            crd::f64 best_sc = 0.0;
            crd::u32 best_ix = 0;
            for (crd::u32 ix = 0; ix < costs.n_indices(); ++ix)
            {
                if (!costs.ix_alive(ix))
                {
                    continue;
                }
                crd::f64 sc = std::log(static_cast<crd::f64>(costs.flop_red(ix)) +
                                       1e-3 * static_cast<crd::f64>(costs.write_red(ix)) + 1.0);
                sc += temperature * hyperdetail::gumbel(rng);
                if (!found || sc > best_sc)
                {
                    found = true;
                    best_sc = sc;
                    best_ix = ix;
                }
            }
            if (!found)
            {
                break; // nothing left to slice
            }
            costs.remove(best_ix);
            if (!chosen.try_push_back(best_ix))
            {
                return HyperStatus::AllocFailed;
            }
        }
        if (costs.max_size() > target_size)
        {
            continue; // this repeat failed the bound
        }
        // rank: (total_flops, nslices, max_size) ascending
        const crd::f64 k0 = costs.total_flops();
        const crd::f64 k1 = costs.nslices();
        const crd::u64 k2 = costs.max_size();
        const bool better = !have_best || k0 < best_key0 || (k0 == best_key0 && k1 < best_key1) ||
                            (k0 == best_key0 && k1 == best_key1 && k2 < best_key2);
        if (better)
        {
            have_best = true;
            best_key0 = k0;
            best_key1 = k1;
            best_key2 = k2;
            out.indices.resize(0);
            for (crd::u32 ix : chosen)
            {
                if (!out.indices.try_push_back(ix))
                {
                    return HyperStatus::AllocFailed;
                }
            }
            out.nslices = static_cast<crd::u64>(k1);
            out.sliced_flops = k0;
            out.overhead = k0 / (costs.original_flops() > 1.0 ? costs.original_flops() : 1.0);
            out.max_size = k2;
        }
    }
    return have_best ? HyperStatus::Ok : HyperStatus::NotFound;
}

// ---------------------------------------------------------------------------
// Labels-partition divisive trees (reference: path_labels.py + the
// PartitionTreeBuilder glue in core.py, python-verified): recursively
// label-propagation-partition the node set; small subgraphs are greedy-filled;
// sibling partitions are glued with best-of random-greedy over the partition
// pseudo-nodes. Everything contracts into the SHARED net (the ssa record IS
// the resulting tree).
// ---------------------------------------------------------------------------
struct HyperLabelsOptions
{
    crd::u32 parts = 4;
    crd::f64 parts_decay = 0.5;
    crd::u32 cutoff = 16;         // <= cutoff nodes: greedy fill
    crd::i32 memory = 0;          // stay-with-current-label bias
    crd::f64 pop_small_bias = 1.0;
    crd::f64 pop_big_bias = 1.0;
    crd::f64 pop_decay = 1.0;
    crd::f64 con_pow = 1.0;
    bool final_sweep = true;
    crd::u32 glue_trials = 8;     // random-greedy trials over partition pseudo-nodes
    crd::u32 max_depth = 64;      // recursion guard (WCET-explicit)
};

namespace hyperdetail
{

// one label-propagation partition of `ids` into <= parts groups; group order
// is deterministic (sorted by label id). Writes group boundaries: nodes are
// permuted in-place so groups are contiguous; group_off gets n_groups+1 marks.
[[nodiscard]] inline HyperStatus labels_partition(const HyperNet& net, crd::containers::Array<crd::u32>& ids,
                                                  crd::u32 id_begin, crd::u32 id_end, crd::u32 parts,
                                                  const HyperLabelsOptions& opt,
                                                  crd::hesap::stats::PhiloxRng& rng,
                                                  crd::containers::Array<crd::u32>& group_off,
                                                  crd::memory::IAllocator* alloc) noexcept
{
    const crd::u32 n = id_end - id_begin;
    group_off.resize(0);
    if (n < 2U || parts < 2U)
    {
        return HyperStatus::BadInput;
    }
    // local position map (net node id -> 0..n-1); planner-scale linear scans
    const auto local_of = [&](crd::u32 node) noexcept -> crd::u32
    {
        for (crd::u32 k = 0; k < n; ++k)
        {
            if (ids[id_begin + k] == node)
            {
                return k;
            }
        }
        return HyperNet::kDead;
    };
    // adjacency: flat (i, j, w) triplets; python assignment semantics = the
    // LAST shared edge's weight wins, then boosted by shared-neighbor count
    struct Adj
    {
        crd::u32 i;
        crd::u32 j;
        crd::f64 w;
    };
    crd::containers::Array<Adj> adj(alloc);
    crd::f64 maxw = 1.0;
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (const HyperLeg& l : net.legs(ids[id_begin + k]))
        {
            const crd::f64 w = std::log2(static_cast<crd::f64>(net.index_size(l.ix))) + 1.0;
            if (w > maxw)
            {
                maxw = w;
            }
        }
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        const crd::u32 node = ids[id_begin + k];
        for (const HyperLeg& l : net.legs(node))
        {
            const crd::f64 w = (std::log2(static_cast<crd::f64>(net.index_size(l.ix))) + 1.0) / maxw;
            net.for_edge_members(l.ix, ~crd::u32{0}, [&](crd::u32 other) noexcept
            {
                if (other == node)
                {
                    return;
                }
                const crd::u32 kj = local_of(other);
                if (kj == HyperNet::kDead)
                {
                    return; // outside this subgraph
                }
                // overwrite-or-insert (i, j)
                for (Adj& a : adj)
                {
                    if (a.i == k && a.j == kj)
                    {
                        a.w = w;
                        return;
                    }
                }
                (void)adj.try_push_back({k, kj, w});
            });
        }
    }
    // shared-neighbor boost: shared(i,j) = 1 + |N(i) ∩ N(j)|
    for (Adj& a : adj)
    {
        crd::u32 shared = 1;
        for (const Adj& x : adj)
        {
            if (x.i != a.i)
            {
                continue;
            }
            for (const Adj& y : adj)
            {
                if (y.i == a.j && y.j == x.j)
                {
                    ++shared;
                    break;
                }
            }
        }
        a.w *= std::pow(static_cast<crd::f64>(shared), opt.con_pow);
    }
    // label propagation
    crd::containers::Array<crd::u32> labels(alloc);
    crd::containers::Array<crd::u32> pops(alloc);
    crd::containers::Array<crd::u32> sites(alloc);
    crd::containers::Array<crd::f64> score(alloc);
    if (!labels.try_reserve(n) || !pops.try_reserve(n) || !sites.try_reserve(n) || !score.try_reserve(n))
    {
        return HyperStatus::AllocFailed;
    }
    labels.resize(n);
    pops.resize(n);
    sites.resize(n);
    score.resize(n);
    for (crd::u32 k = 0; k < n; ++k)
    {
        labels[k] = k;
        pops[k] = 1;
        sites[k] = k;
    }
    const crd::f64 m = static_cast<crd::f64>(n) / static_cast<crd::f64>(parts);
    for (crd::u32 r = 0; r < n; ++r)
    {
        // Fisher-Yates shuffle
        for (crd::u32 k = n; k > 1U; --k)
        {
            const crd::u32 j = static_cast<crd::u32>(rng.next_below(k));
            const crd::u32 t = sites[k - 1U];
            sites[k - 1U] = sites[j];
            sites[j] = t;
        }
        bool moved = false;
        const crd::f64 decay = std::pow(static_cast<crd::f64>(r + 1U), opt.pop_decay);
        for (crd::u32 si = 0; si < n; ++si)
        {
            const crd::u32 i = sites[si];
            const crd::u32 old = labels[i];
            for (crd::u32 k = 0; k < n; ++k)
            {
                score[k] = -1e300;
            }
            score[old] = static_cast<crd::f64>(opt.memory);
            for (const Adj& a : adj)
            {
                if (a.i != i)
                {
                    continue;
                }
                const crd::u32 lbl = labels[a.j];
                if (score[lbl] < -1e299)
                {
                    score[lbl] = 0.0;
                }
                score[lbl] += a.w;
            }
            for (crd::u32 lbl = 0; lbl < n; ++lbl)
            {
                if (score[lbl] < -1e299)
                {
                    continue;
                }
                const crd::f64 p = static_cast<crd::f64>(pops[lbl]);
                crd::f64 bias;
                if (p <= m)
                {
                    bias = opt.pop_small_bias * static_cast<crd::f64>(n) * std::sin(3.14159265358979323846 * p / m);
                }
                else
                {
                    bias = -opt.pop_big_bias * static_cast<crd::f64>(n) *
                           std::sin(3.14159265358979323846 * 0.5 * (p - m) /
                                    (static_cast<crd::f64>(n) - m > 1e-12 ? static_cast<crd::f64>(n) - m : 1e-12));
                }
                score[lbl] += bias / decay;
            }
            // argmax; ties prefer the OLD label (reference Counter order)
            crd::u32 best = old;
            for (crd::u32 lbl = 0; lbl < n; ++lbl)
            {
                if (score[lbl] < -1e299)
                {
                    continue;
                }
                if (score[lbl] > score[best] || (score[lbl] == score[best] && lbl == old))
                {
                    best = lbl;
                }
            }
            if (best != old)
            {
                moved = true;
                --pops[old];
                ++pops[best];
                labels[i] = best;
            }
        }
        if (!moved)
        {
            break;
        }
    }
    if (opt.final_sweep)
    {
        for (crd::u32 k = n; k > 1U; --k)
        {
            const crd::u32 j = static_cast<crd::u32>(rng.next_below(k));
            const crd::u32 t = sites[k - 1U];
            sites[k - 1U] = sites[j];
            sites[j] = t;
        }
        for (crd::u32 si = 0; si < n; ++si)
        {
            const crd::u32 i = sites[si];
            for (crd::u32 k = 0; k < n; ++k)
            {
                score[k] = -1e300;
            }
            score[labels[i]] = 0.0;
            for (const Adj& a : adj)
            {
                if (a.i != i)
                {
                    continue;
                }
                if (score[labels[a.j]] < -1e299)
                {
                    score[labels[a.j]] = 0.0;
                }
                score[labels[a.j]] += a.w;
            }
            crd::u32 best = labels[i];
            for (crd::u32 lbl = 0; lbl < n; ++lbl)
            {
                if (score[lbl] > -1e299 && score[lbl] > score[best])
                {
                    best = lbl;
                }
            }
            labels[i] = best;
        }
    }
    // group by label: stable permute ids so groups are contiguous, label-sorted
    crd::containers::Array<crd::u32> scratch(alloc);
    if (!scratch.try_reserve(n))
    {
        return HyperStatus::AllocFailed;
    }
    scratch.resize(n);
    if (!group_off.try_push_back(0U))
    {
        return HyperStatus::AllocFailed;
    }
    crd::u32 w = 0;
    for (crd::u32 lbl = 0; lbl < n; ++lbl)
    {
        bool any = false;
        for (crd::u32 k = 0; k < n; ++k)
        {
            if (labels[k] == lbl)
            {
                scratch[w++] = ids[id_begin + k];
                any = true;
            }
        }
        if (any)
        {
            if (!group_off.try_push_back(w))
            {
                return HyperStatus::AllocFailed;
            }
        }
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        ids[id_begin + k] = scratch[k];
    }
    return HyperStatus::Ok;
}

} // namespace hyperdetail

namespace hyperdetail
{

struct DivideCtx
{
    HyperNet& net;
    const HyperLabelsOptions& opt;
    crd::hesap::stats::PhiloxRng rng;
    crd::containers::Array<crd::u32>& ids;
    crd::memory::IAllocator* alloc;
    crd::u32 total_n;
};

// T=0 greedy over the segment's alive set only (small: <= cutoff); leftovers
// pair by size. Returns the surviving node id (kDead = allocation failure).
[[nodiscard]] inline crd::u32 divide_greedy_fill(DivideCtx& c, crd::u32 b, crd::u32 e) noexcept
{
    crd::containers::Array<crd::u32> alive(c.alloc);
    for (crd::u32 k = b; k < e; ++k)
    {
        if (!alive.try_push_back(c.ids[k]))
        {
            return HyperNet::kDead;
        }
    }
    const auto is_alive = [&](crd::u32 node) noexcept -> bool
    {
        for (crd::u32 x : alive)
        {
            if (x == node)
            {
                return true;
            }
        }
        return false;
    };
    const auto drop = [&](crd::u32 node) noexcept
    {
        for (crd::usize k = 0; k < alive.size(); ++k)
        {
            if (alive[k] == node)
            {
                alive[k] = alive[alive.size() - 1U];
                alive.pop_back();
                return;
            }
        }
    };
    while (alive.size() > 1U)
    {
        // best shared-edge pair by "size-added minus size-removed" (T=0 local
        // greedy score with costmod 1)
        bool found = false;
        crd::f64 best_sc = 0.0;
        crd::u32 bi = 0;
        crd::u32 bj = 0;
        for (crd::usize x = 0; x < alive.size(); ++x)
        {
            const crd::u32 i = alive[x];
            c.net.for_neighbors_limit(i, 16U, [&](crd::u32 j) noexcept
            {
                if (j <= i || !is_alive(j))
                {
                    return;
                }
                const auto li = c.net.legs(i);
                const auto lj = c.net.legs(j);
                // merged size without materializing
                crd::u64 s = 1;
                crd::u32 p = 0;
                crd::u32 q = 0;
                while (p < li.size() && q < lj.size())
                {
                    if (li[p].ix < lj[q].ix)
                    {
                        s = sat_mul(s, c.net.index_size(li[p].ix));
                        ++p;
                    }
                    else if (lj[q].ix < li[p].ix)
                    {
                        s = sat_mul(s, c.net.index_size(lj[q].ix));
                        ++q;
                    }
                    else
                    {
                        if (li[p].count + lj[q].count < c.net.appearances(li[p].ix))
                        {
                            s = sat_mul(s, c.net.index_size(li[p].ix));
                        }
                        ++p;
                        ++q;
                    }
                }
                for (; p < li.size(); ++p)
                {
                    s = sat_mul(s, c.net.index_size(li[p].ix));
                }
                for (; q < lj.size(); ++q)
                {
                    s = sat_mul(s, c.net.index_size(lj[q].ix));
                }
                const crd::f64 sc = static_cast<crd::f64>(s) -
                                    (static_cast<crd::f64>(c.net.size_of(li)) +
                                     static_cast<crd::f64>(c.net.size_of(lj)));
                if (!found || sc < best_sc || (sc == best_sc && (i < bi || (i == bi && j < bj))))
                {
                    found = true;
                    best_sc = sc;
                    bi = i;
                    bj = j;
                }
            });
        }
        if (!found)
        {
            // disconnected leftovers: two smallest by size (id tiebreak)
            crd::u32 s1 = HyperNet::kDead;
            crd::u32 s2 = HyperNet::kDead;
            crd::u64 z1 = kSat;
            crd::u64 z2 = kSat;
            for (crd::u32 x : alive)
            {
                const crd::u64 z = c.net.size_of(c.net.legs(x));
                if (z < z1 || (z == z1 && x < s1))
                {
                    s2 = s1;
                    z2 = z1;
                    s1 = x;
                    z1 = z;
                }
                else if (z < z2 || (z == z2 && x < s2))
                {
                    s2 = x;
                    z2 = z;
                }
            }
            bi = s1;
            bj = s2;
        }
        const crd::u32 k2 = c.net.contract(bi, bj);
        if (k2 == HyperNet::kDead)
        {
            return HyperNet::kDead;
        }
        drop(bi);
        drop(bj);
        if (!alive.try_push_back(k2))
        {
            return HyperNet::kDead;
        }
    }
    return alive[0];
}

// glue sibling partition representatives: best-of glue_trials random-greedy
// orders evaluated on scratch legs, the winner applied to the net.
[[nodiscard]] inline crd::u32 divide_glue(DivideCtx& c, crd::containers::Array<crd::u32>& reps,
                                          crd::u64 glue_seed) noexcept
{
    const crd::u32 k = static_cast<crd::u32>(reps.size());
    if (k == 1U)
    {
        return reps[0];
    }
    if (k == 2U)
    {
        return c.net.contract(reps[0], reps[1]);
    }
    // scratch legs per pseudo-node (evolving); order recorded as local pairs
    crd::containers::Array<HyperLeg> pool(c.alloc);
    crd::containers::Array<crd::u32> off(c.alloc);
    crd::containers::Array<crd::u32> len(c.alloc);
    crd::containers::Array<crd::u8> live(c.alloc);
    crd::containers::Array<crd::u32> order(c.alloc);      // 2 entries per step
    crd::containers::Array<crd::u32> best_order(c.alloc);
    crd::f64 best_cost = -1.0;
    const crd::u32 trials = c.opt.glue_trials > 0U ? c.opt.glue_trials : 1U;
    for (crd::u32 t = 0; t < trials; ++t)
    {
        crd::hesap::stats::PhiloxRng trng(glue_seed, t);
        const crd::f64 cm = 0.1 + (4.0 - 0.1) * trng.next_f64();
        const crd::f64 tp = std::exp(std::log(0.001) + (std::log(1.0) - std::log(0.001)) * trng.next_f64());
        pool.resize(0);
        off.resize(0);
        len.resize(0);
        live.resize(0);
        order.resize(0);
        for (crd::u32 x = 0; x < k; ++x)
        {
            const auto l = c.net.legs(reps[x]);
            if (!off.try_push_back(static_cast<crd::u32>(pool.size())) ||
                !len.try_push_back(static_cast<crd::u32>(l.size())) || !live.try_push_back(1U))
            {
                return HyperNet::kDead;
            }
            for (const HyperLeg& lg : l)
            {
                if (!pool.try_push_back(lg))
                {
                    return HyperNet::kDead;
                }
            }
        }
        crd::f64 cost = 0.0;
        crd::u32 remaining = k;
        while (remaining > 1U)
        {
            bool found = false;
            crd::f64 bsc = 0.0;
            crd::u32 bi = 0;
            crd::u32 bj = 0;
            for (crd::u32 i = 0; i < live.size(); ++i)
            {
                if (live[i] == 0U)
                {
                    continue;
                }
                for (crd::u32 j = i + 1U; j < live.size(); ++j)
                {
                    if (live[j] == 0U)
                    {
                        continue;
                    }
                    const crd::containers::ConstSpan<HyperLeg> li{pool.data() + off[i], len[i]};
                    const crd::containers::ConstSpan<HyperLeg> lj{pool.data() + off[j], len[j]};
                    crd::u64 sab = 1;
                    crd::u32 p = 0;
                    crd::u32 q = 0;
                    while (p < li.size() && q < lj.size())
                    {
                        if (li[p].ix < lj[q].ix)
                        {
                            sab = sat_mul(sab, c.net.index_size(li[p].ix));
                            ++p;
                        }
                        else if (lj[q].ix < li[p].ix)
                        {
                            sab = sat_mul(sab, c.net.index_size(lj[q].ix));
                            ++q;
                        }
                        else
                        {
                            if (li[p].count + lj[q].count < c.net.appearances(li[p].ix))
                            {
                                sab = sat_mul(sab, c.net.index_size(li[p].ix));
                            }
                            ++p;
                            ++q;
                        }
                    }
                    for (; p < li.size(); ++p)
                    {
                        sab = sat_mul(sab, c.net.index_size(li[p].ix));
                    }
                    for (; q < lj.size(); ++q)
                    {
                        sab = sat_mul(sab, c.net.index_size(lj[q].ix));
                    }
                    const crd::u64 sa = c.net.size_of(li);
                    const crd::u64 sb = c.net.size_of(lj);
                    const crd::f64 s = static_cast<crd::f64>(sab) / cm -
                                       (static_cast<crd::f64>(sa) + static_cast<crd::f64>(sb)) * cm;
                    crd::f64 base = 0.0;
                    if (s > 0.0)
                    {
                        base = std::log(s);
                    }
                    else if (s < 0.0)
                    {
                        base = -std::log(-s);
                    }
                    const crd::f64 sc = base - tp * gumbel(trng);
                    if (!found || sc < bsc)
                    {
                        found = true;
                        bsc = sc;
                        bi = i;
                        bj = j;
                    }
                }
            }
            if (!found)
            {
                break;
            }
            // merge bi, bj into a fresh pseudo node
            const crd::containers::ConstSpan<HyperLeg> li{pool.data() + off[bi], len[bi]};
            const crd::containers::ConstSpan<HyperLeg> lj{pool.data() + off[bj], len[bj]};
            cost += static_cast<crd::f64>(c.net.flops_of(li, lj));
            // materialize merged legs at the pool tail (reserve first: spans!)
            if (!pool.try_reserve(pool.size() + li.size() + lj.size()))
            {
                return HyperNet::kDead;
            }
            const crd::u32 moff = static_cast<crd::u32>(pool.size());
            crd::u32 mlen = 0;
            {
                const crd::containers::ConstSpan<HyperLeg> ra{pool.data() + off[bi], len[bi]};
                const crd::containers::ConstSpan<HyperLeg> rb{pool.data() + off[bj], len[bj]};
                crd::u32 p = 0;
                crd::u32 q = 0;
                const auto push = [&](HyperLeg lg) noexcept -> bool
                {
                    if (!pool.try_push_back(lg))
                    {
                        return false;
                    }
                    ++mlen;
                    return true;
                };
                bool ok = true;
                while (p < ra.size() && q < rb.size())
                {
                    if (ra[p].ix < rb[q].ix)
                    {
                        ok = ok && push(ra[p++]);
                    }
                    else if (rb[q].ix < ra[p].ix)
                    {
                        ok = ok && push(rb[q++]);
                    }
                    else
                    {
                        const crd::u32 cc = ra[p].count + rb[q].count;
                        if (cc < c.net.appearances(ra[p].ix))
                        {
                            ok = ok && push({ra[p].ix, cc});
                        }
                        ++p;
                        ++q;
                    }
                }
                for (; p < ra.size(); ++p)
                {
                    ok = ok && push(ra[p]);
                }
                for (; q < rb.size(); ++q)
                {
                    ok = ok && push(rb[q]);
                }
                if (!ok)
                {
                    return HyperNet::kDead;
                }
            }
            live[bi] = 0U;
            live[bj] = 0U;
            if (!off.try_push_back(moff) || !len.try_push_back(mlen) || !live.try_push_back(1U) ||
                !order.try_push_back(bi) || !order.try_push_back(bj))
            {
                return HyperNet::kDead;
            }
            --remaining;
        }
        if (best_cost < 0.0 || cost < best_cost)
        {
            best_cost = cost;
            best_order.resize(0);
            for (crd::u32 v : order)
            {
                if (!best_order.try_push_back(v))
                {
                    return HyperNet::kDead;
                }
            }
        }
    }
    // apply the winning order to the net: local pseudo ids 0..k-1 then merges
    crd::containers::Array<crd::u32> real(c.alloc);
    for (crd::u32 x = 0; x < k; ++x)
    {
        if (!real.try_push_back(reps[x]))
        {
            return HyperNet::kDead;
        }
    }
    for (crd::usize s = 0; s + 1U < best_order.size(); s += 2U)
    {
        const crd::u32 node = c.net.contract(real[best_order[s]], real[best_order[s + 1U]]);
        if (node == HyperNet::kDead)
        {
            return HyperNet::kDead;
        }
        if (!real.try_push_back(node))
        {
            return HyperNet::kDead;
        }
    }
    return real[real.size() - 1U];
}

[[nodiscard]] inline crd::u32 divide_solve(DivideCtx& c, crd::u32 b, crd::u32 e, crd::u32 depth) noexcept
{
    const crd::u32 n = e - b;
    if (n == 1U)
    {
        return c.ids[b];
    }
    if (n == 2U)
    {
        return c.net.contract(c.ids[b], c.ids[b + 1U]);
    }
    if (n <= c.opt.cutoff || depth >= c.opt.max_depth)
    {
        return divide_greedy_fill(c, b, e);
    }
    const crd::f64 s = static_cast<crd::f64>(n) / static_cast<crd::f64>(c.total_n);
    crd::u32 parts_s = static_cast<crd::u32>(std::pow(s, c.opt.parts_decay) * static_cast<crd::f64>(c.opt.parts));
    if (parts_s < 2U)
    {
        parts_s = 2U;
    }
    crd::containers::Array<crd::u32> group_off(c.alloc);
    if (labels_partition(c.net, c.ids, b, e, parts_s, c.opt, c.rng, group_off, c.alloc) != HyperStatus::Ok ||
        group_off.size() < 3U) // 1 group (2 marks) = no split found
    {
        return divide_greedy_fill(c, b, e);
    }
    crd::containers::Array<crd::u32> reps(c.alloc);
    for (crd::usize g = 0; g + 1U < group_off.size(); ++g)
    {
        const crd::u32 rep = divide_solve(c, b + group_off[g], b + group_off[g + 1U], depth + 1U);
        if (rep == HyperNet::kDead)
        {
            return HyperNet::kDead;
        }
        if (!reps.try_push_back(rep))
        {
            return HyperNet::kDead;
        }
    }
    return divide_glue(c, reps, (c.rng.next_u64() << 8) ^ depth);
}

} // namespace hyperdetail

// Labels-divide trial: partition-tree construction over the whole net.
// Contracts everything; the net's SSA record is the resulting tree.
[[nodiscard]] inline HyperStatus hyper_labels_divide(hyperdetail::HyperNet& net, const HyperLabelsOptions& opt,
                                                     crd::u64 seed, crd::u64 stream) noexcept
{
    crd::containers::Array<crd::u32> ids(net.allocator());
    for (crd::u32 i = 0; i < net.ssa_next(); ++i)
    {
        if (net.alive(i))
        {
            if (!ids.try_push_back(i))
            {
                return HyperStatus::AllocFailed;
            }
        }
    }
    if (ids.size() < 2U)
    {
        return ids.size() == 1U ? HyperStatus::Ok : HyperStatus::BadInput;
    }
    hyperdetail::DivideCtx ctx{net, opt, crd::hesap::stats::PhiloxRng(seed, stream), ids, net.allocator(),
                               static_cast<crd::u32>(ids.size())};
    const crd::u32 root = hyperdetail::divide_solve(ctx, 0U, static_cast<crd::u32>(ids.size()), 0U);
    if (root == hyperdetail::HyperNet::kDead)
    {
        return HyperStatus::AllocFailed;
    }
    return net.n_alive() == 1U ? HyperStatus::Ok : HyperStatus::BadInput;
}

// ---------------------------------------------------------------------------
// The DRIVER: deterministic hyper-optimization over {greedy, labels-divide}
// trials (stratified Philox-keyed parameters), per-trial subtree-reconfigure,
// treesa polish on the best finalists, optional exact-bound slicing. Trials
// are the parallel unit over crd-jobs: trial t's result depends ONLY on
// (seed, t) ⇒ the outcome is BIT-IDENTICAL at any worker count (the moat
// cotengra structurally lacks: unseeded TPE + global-RNG trial fns +
// completion-order feedback).
// ---------------------------------------------------------------------------
struct HyperOptOptions
{
    crd::u32 ntrials = 64;
    crd::u32 sa_finalists = 3;
    HyperObjective objective = HyperObjective::Flops;
    crd::f64 combo_factor = 64.0;
    crd::u32 reconf_subtree = 8;
    crd::u32 reconf_maxiter = 256;
    crd::u64 seed = 0;
    bool parallel = true;
    crd::f64 sa_tstart = 1.0;
    crd::f64 sa_tfinal = 0.02;
    crd::u32 sa_tsteps = 30;
    crd::u32 sa_numiter = 30;
    crd::u64 target_size = 0; // 0 = no slicing; else the winner is sliced to fit EXACTLY
    HyperLabelsOptions labels;
};

struct HyperOptResult
{
    HyperPlan plan;
    HyperTreeStats stats;
    HyperSliceResult slicing;
    crd::u32 winner_trial = 0;
    bool winner_used_sa = false;
    bool sliced = false;
    explicit HyperOptResult(crd::memory::IAllocator* alloc) : plan(alloc), slicing(alloc) {}
};

namespace hyperdetail
{

// one trial: clone → method → tree → reconfigure → score. Outputs are written
// into per-trial slots (disjoint across trials — safe under parallel_for).
inline void hyper_run_trial(const HyperNet& net0, crd::u32 t, crd::u32 half, const HyperOptOptions& opt,
                            crd::memory::IAllocator* alloc, crd::f64* score_out, crd::u32* ssa_out,
                            HyperTreeStats* stats_out) noexcept
{
    *score_out = 1e300;
    HyperNet net(alloc);
    if (net.clone_from(net0) != HyperStatus::Ok)
    {
        return;
    }
    const crd::u32 n = net0.n_leaves();
    if (t < half)
    {
        crd::hesap::stats::PhiloxRng prng(opt.seed, 4ULL * t);
        crd::hesap::stats::PhiloxRng grng(opt.seed, 4ULL * t + 1ULL);
        HyperGreedyOptions g;
        // stratified costmod over the greedy half; log-uniform temperature
        const crd::f64 u0 = (static_cast<crd::f64>(t) + prng.next_f64()) / static_cast<crd::f64>(half);
        g.costmod = 0.1 + (4.0 - 0.1) * (u0 < 1.0 ? u0 : 0.999999);
        g.temperature = std::exp(std::log(0.001) + (std::log(1.0) - std::log(0.001)) * prng.next_f64());
        if (hyper_greedy(net, g, grng, 0.0) != HyperStatus::Ok)
        {
            return;
        }
    }
    else
    {
        const crd::u32 k = t - half;
        crd::hesap::stats::PhiloxRng prng(opt.seed, 4ULL * t);
        HyperLabelsOptions lo = opt.labels;
        lo.parts = 2U + (k % 8U);
        lo.parts_decay = prng.next_f64();
        lo.con_pow = 3.0 * prng.next_f64();
        if (hyper_labels_divide(net, lo, opt.seed, 4ULL * t + 2ULL) != HyperStatus::Ok)
        {
            return;
        }
    }
    HyperTree tree(alloc);
    if (tree.build(net) != HyperStatus::Ok)
    {
        return;
    }
    if (tree.reconfigure(opt.reconf_subtree, opt.reconf_maxiter, opt.objective, opt.combo_factor) != HyperStatus::Ok)
    {
        return;
    }
    const HyperTreeStats st = tree.stats();
    // record the RECONFIGURED tree's ssa (post-order emission)
    crd::u32 w = 0;
    {
        crd::containers::Array<crd::u32> stack(alloc);
        crd::containers::Array<crd::u32> ssa_of(alloc);
        if (!ssa_of.try_reserve(tree.n_nodes()) || !stack.try_reserve(2U * tree.n_nodes()))
        {
            return;
        }
        ssa_of.resize(tree.n_nodes(), HyperNet::kDead);
        for (crd::u32 leaf = 0; leaf < n; ++leaf)
        {
            ssa_of[leaf] = leaf;
        }
        crd::u32 next_ssa = n;
        // iterative post-order from the root
        if (!stack.try_push_back(tree.root()))
        {
            return;
        }
        while (stack.size() > 0U)
        {
            const crd::u32 p = stack[stack.size() - 1U];
            if (!tree.is_internal(p) || ssa_of[p] != HyperNet::kDead)
            {
                stack.pop_back();
                continue;
            }
            const crd::u32 a = tree.child_a(p);
            const crd::u32 b = tree.child_b(p);
            const bool a_ready = !tree.is_internal(a) || ssa_of[a] != HyperNet::kDead;
            const bool b_ready = !tree.is_internal(b) || ssa_of[b] != HyperNet::kDead;
            if (!a_ready)
            {
                if (!stack.try_push_back(a))
                {
                    return;
                }
                continue;
            }
            if (!b_ready)
            {
                if (!stack.try_push_back(b))
                {
                    return;
                }
                continue;
            }
            stack.pop_back();
            if (w >= n - 1U) // HARD bound: never write past this trial's slab slot
            {
                return;
            }
            ssa_out[2U * w] = ssa_of[a];
            ssa_out[2U * w + 1U] = ssa_of[b];
            ++w;
            ssa_of[p] = next_ssa++;
        }
    }
    if (w != n - 1U)
    {
        return; // malformed tree — leave the trial failed rather than lie
    }
    *stats_out = st;
    *score_out = hyper_score_tree(opt.objective, st, opt.combo_factor);
}

} // namespace hyperdetail

// Optimize the contraction order of a network given as operand index-id lists.
// Deterministic for a fixed (opts.seed) at ANY worker count. When
// opts.target_size > 0 the winning tree is sliced to fit EXACTLY (NotFound if
// unreachable — never a silent best-effort).
[[nodiscard]] inline HyperStatus hyper_optimize(
    crd::containers::ConstSpan<crd::containers::ConstSpan<crd::u32>> ids,
    crd::containers::ConstSpan<crd::u32> out_ids, crd::containers::ConstSpan<crd::u64> sizes,
    const HyperOptOptions& opts, crd::memory::IAllocator* alloc, HyperOptResult& result) noexcept
{
    using hyperdetail::HyperNet;
    if (opts.ntrials < 2U)
    {
        return HyperStatus::BadInput;
    }
    HyperNet net0(alloc);
    {
        const HyperStatus st = net0.build(ids, out_ids, sizes);
        if (st != HyperStatus::Ok)
        {
            return st;
        }
    }
    const crd::u32 n = net0.n_leaves();
    if (n < 2U)
    {
        return HyperStatus::BadInput;
    }
    const crd::u32 steps = n - 1U;
    const crd::u32 half = opts.ntrials / 2U;
    // per-trial slots (disjoint writes under parallel_for)
    crd::containers::Array<crd::f64> scores(alloc);
    crd::containers::Array<HyperTreeStats> tstats(alloc);
    crd::containers::Array<crd::u32> ssa_slab(alloc);
    if (!scores.try_reserve(opts.ntrials) || !tstats.try_reserve(opts.ntrials) ||
        !ssa_slab.try_reserve(2U * static_cast<crd::usize>(steps) * opts.ntrials))
    {
        return HyperStatus::AllocFailed;
    }
    scores.resize(opts.ntrials, 1e300);
    tstats.resize(opts.ntrials);
    ssa_slab.resize(2U * static_cast<crd::usize>(steps) * opts.ntrials, 0U);
    const bool go_parallel = opts.parallel && crd::jobs::num_workers() > 1U;
    if (go_parallel)
    {
        crd::memory::ThreadSafeAllocator ts(alloc);
        struct Ctx
        {
            const HyperNet* net0;
            const HyperOptOptions* opts;
            crd::memory::ThreadSafeAllocator* ts;
            crd::f64* scores;
            HyperTreeStats* tstats;
            crd::u32* ssa;
            crd::u32 steps;
            crd::u32 half;
        };
        Ctx ctx{&net0, &opts, &ts, scores.data(), tstats.data(), ssa_slab.data(), steps, half};
        Ctx* const cp = &ctx;
        const crd::u32 nj = crd::jobs::num_workers();
        auto* const counter = crd::jobs::parallel_for(opts.ntrials, nj, [cp](crd::u32 begin, crd::u32 end) {
            for (crd::u32 t = begin; t < end; ++t)
            {
                hyperdetail::hyper_run_trial(*cp->net0, t, cp->half, *cp->opts, cp->ts, cp->scores + t,
                                             cp->ssa + 2U * static_cast<crd::usize>(cp->steps) * t, cp->tstats + t);
            }
        });
        crd::jobs::wait(counter);
    }
    else
    {
        for (crd::u32 t = 0; t < opts.ntrials; ++t)
        {
            hyperdetail::hyper_run_trial(net0, t, half, opts, alloc, scores.data() + t,
                                         ssa_slab.data() + 2U * static_cast<crd::usize>(steps) * t,
                                         tstats.data() + t);
        }
    }
    // deterministic reduction: best + the top-K finalists by (score, trial)
    crd::u32 order[16];
    const crd::u32 kfin = opts.sa_finalists < 16U ? opts.sa_finalists : 16U;
    crd::u32 nfin = 0;
    for (crd::u32 t = 0; t < opts.ntrials; ++t)
    {
        if (scores[t] >= 1e300)
        {
            continue;
        }
        crd::u32 pos = nfin;
        while (pos > 0U && scores[order[pos - 1U]] > scores[t])
        {
            --pos;
        }
        if (pos < kfin)
        {
            for (crd::u32 m = (nfin < kfin ? nfin : kfin - 1U); m > pos; --m)
            {
                order[m] = order[m - 1U];
            }
            order[pos] = t;
            if (nfin < kfin)
            {
                ++nfin;
            }
        }
    }
    if (nfin == 0U)
    {
        return HyperStatus::NotFound; // every trial failed
    }
    // SA-polish the finalists to find the winner CONFIG; the winning tree is
    // then re-derived deterministically (replay + same-seed anneal) — no tree
    // moves/copies, and the re-derivation is bit-identical by construction.
    crd::f64 best_score = scores[order[0]];
    crd::u32 best_trial = order[0];
    crd::u32 best_rank = 0;
    bool best_sa = false;
    const auto rebuild = [&](crd::u32 trial, bool with_sa, crd::u32 rank, HyperTree& tree) noexcept -> HyperStatus
    {
        HyperNet net(alloc);
        if (net.clone_from(net0) != HyperStatus::Ok)
        {
            return HyperStatus::AllocFailed;
        }
        const crd::u32* ssa = ssa_slab.data() + 2U * static_cast<crd::usize>(steps) * trial;
        for (crd::u32 k = 0; k < steps; ++k)
        {
            const crd::u32 a = ssa[2U * k];
            const crd::u32 b = ssa[2U * k + 1U];
            if (a >= net.ssa_next() || b >= net.ssa_next() || !net.alive(a) || !net.alive(b))
            {
                return HyperStatus::BadInput; // slab invariant — a recorded pair must replay
            }
            if (net.contract(a, b) == HyperNet::kDead)
            {
                return HyperStatus::AllocFailed;
            }
        }
        HyperStatus st = tree.build(net);
        if (st != HyperStatus::Ok)
        {
            return st;
        }
        if (with_sa)
        {
            st = tree.anneal(opts.sa_tstart, opts.sa_tfinal, opts.sa_tsteps, opts.sa_numiter, opts.objective,
                             opts.combo_factor, opts.seed, 0x5AULL << 32 | rank);
            if (st != HyperStatus::Ok)
            {
                return st;
            }
            st = tree.reconfigure(opts.reconf_subtree, opts.reconf_maxiter, opts.objective, opts.combo_factor);
            if (st != HyperStatus::Ok)
            {
                return st;
            }
        }
        return HyperStatus::Ok;
    };
    for (crd::u32 r = 0; r < nfin && opts.sa_finalists > 0U; ++r)
    {
        HyperTree tree(alloc);
        if (rebuild(order[r], true, r, tree) != HyperStatus::Ok)
        {
            return HyperStatus::AllocFailed;
        }
        const HyperTreeStats st = tree.stats();
        const crd::f64 sc = hyper_score_tree(opts.objective, st, opts.combo_factor);
        if (sc < best_score)
        {
            best_score = sc;
            best_trial = order[r];
            best_rank = r;
            best_sa = true;
        }
    }
    HyperTree best_tree(alloc);
    if (rebuild(best_trial, best_sa, best_rank, best_tree) != HyperStatus::Ok)
    {
        return HyperStatus::AllocFailed;
    }
    // emit the plan from the best tree (post-order ssa + per-step costs)
    result.stats = best_tree.stats();
    result.winner_trial = best_trial;
    result.winner_used_sa = best_sa;
    result.plan.steps.resize(0);
    result.plan.total_flops = result.stats.flops;
    result.plan.max_size = result.stats.size;
    {
        crd::containers::Array<crd::u32> stack(alloc);
        crd::containers::Array<crd::u32> ssa_of(alloc);
        if (!ssa_of.try_reserve(best_tree.n_nodes()) || !stack.try_reserve(2U * best_tree.n_nodes()))
        {
            return HyperStatus::AllocFailed;
        }
        ssa_of.resize(best_tree.n_nodes(), HyperNet::kDead);
        for (crd::u32 leaf = 0; leaf < n; ++leaf)
        {
            ssa_of[leaf] = leaf;
        }
        crd::u32 next_ssa = n;
        if (!stack.try_push_back(best_tree.root()))
        {
            return HyperStatus::AllocFailed;
        }
        while (stack.size() > 0U)
        {
            const crd::u32 p = stack[stack.size() - 1U];
            if (!best_tree.is_internal(p) || ssa_of[p] != HyperNet::kDead)
            {
                stack.pop_back();
                continue;
            }
            const crd::u32 a = best_tree.child_a(p);
            const crd::u32 b = best_tree.child_b(p);
            const bool a_ready = !best_tree.is_internal(a) || ssa_of[a] != HyperNet::kDead;
            const bool b_ready = !best_tree.is_internal(b) || ssa_of[b] != HyperNet::kDead;
            if (!a_ready)
            {
                if (!stack.try_push_back(a))
                {
                    return HyperStatus::AllocFailed;
                }
                continue;
            }
            if (!b_ready)
            {
                if (!stack.try_push_back(b))
                {
                    return HyperStatus::AllocFailed;
                }
                continue;
            }
            stack.pop_back();
            HyperStep step;
            step.a = ssa_of[a];
            step.b = ssa_of[b];
            step.flops = best_tree.node_flops(p);
            step.out_size = best_tree.node_size(p);
            if (!result.plan.steps.try_push_back(step))
            {
                return HyperStatus::AllocFailed;
            }
            ssa_of[p] = next_ssa++;
        }
    }
    result.sliced = false;
    if (opts.target_size > 0U)
    {
        const HyperStatus st =
            hyper_slice(best_tree, net0, opts.target_size, opts.seed, 16U, 0.01, result.slicing);
        if (st != HyperStatus::Ok)
        {
            return st; // NotFound propagates — the bound is a contract
        }
        result.sliced = true;
    }
    return HyperStatus::Ok;
}

inline HyperStatus HyperTree::anneal(crd::f64 tstart, crd::f64 tfinal, crd::u32 tsteps, crd::u32 numiter,
                                     HyperObjective obj, crd::f64 factor, crd::u64 seed, crd::u64 stream) noexcept
{
    if (tsteps == 0U || tstart <= 0.0 || tfinal <= 0.0)
    {
        return HyperStatus::BadInput;
    }
    crd::hesap::stats::PhiloxRng rng(seed, stream);
    const crd::f64 cf = obj == HyperObjective::Combo ? factor : 0.0;
    const auto local_score = [&](crd::u64 f0, crd::u64 f1, crd::u64 s0, crd::u64 s1) noexcept -> crd::f64
    {
        switch (obj)
        {
        case HyperObjective::Size:
        {
            const crd::u64 mx = s0 > s1 ? s0 : s1;
            return std::log2(static_cast<crd::f64>(mx > 1U ? mx : 1U));
        }
        case HyperObjective::Flops:
        {
            const crd::f64 fs = static_cast<crd::f64>(f0) + static_cast<crd::f64>(f1);
            return std::log2(fs > 1.0 ? fs : 1.0);
        }
        case HyperObjective::Combo:
        default:
        {
            const crd::f64 v = static_cast<crd::f64>(f0) + static_cast<crd::f64>(f1) +
                               cf * (static_cast<crd::f64>(s0) + static_cast<crd::f64>(s1));
            return std::log2(v > 1.0 ? v : 1.0);
        }
        }
    };
    crd::containers::Array<crd::u32> queue(m_alloc);
    for (crd::u32 step = 0; step < tsteps; ++step)
    {
        crd::f64 temp = tstart;
        if (tsteps > 1U)
        {
            const crd::f64 l0 = std::log2(tstart);
            const crd::f64 l1 = std::log2(tfinal);
            temp = std::exp2(l0 + static_cast<crd::f64>(step) * (l1 - l0) / static_cast<crd::f64>(tsteps - 1U));
        }
        for (crd::u32 it = 0; it < numiter; ++it)
        {
            queue.resize(0);
            if (!queue.try_push_back(m_root))
            {
                return HyperStatus::AllocFailed;
            }
            crd::u32 head = 0;
            while (head < queue.size())
            {
                const crd::u32 p = queue[head++];
                if (!is_internal(p))
                {
                    continue;
                }
                const crd::u32 l = m_child_a[p];
                const crd::u32 r = m_child_b[p];
                const bool lleaf = !is_internal(l);
                const bool rleaf = !is_internal(r);
                if (!(lleaf && rleaf))
                {
                    crd::u32 rule;
                    if (lleaf)
                    {
                        rule = 2U + static_cast<crd::u32>(rng.next_u64() & 1U);
                    }
                    else if (rleaf)
                    {
                        rule = static_cast<crd::u32>(rng.next_u64() & 1U);
                    }
                    else
                    {
                        rule = static_cast<crd::u32>(rng.next_u64() & 3U);
                    }
                    crd::u32 x;
                    crd::u32 n0;
                    crd::u32 n1;
                    crd::u32 n2;
                    if (rule <= 1U)
                    {
                        x = l; // ((A B) C): a,b = children(x), c = r
                        const crd::u32 a = m_child_a[x];
                        const crd::u32 b = m_child_b[x];
                        n0 = rule == 0U ? a : b;
                        n1 = r;
                        n2 = rule == 0U ? b : a;
                    }
                    else
                    {
                        x = r; // (A (B C)): b,c = children(x), a = l
                        const crd::u32 b = m_child_a[x];
                        const crd::u32 c = m_child_b[x];
                        n0 = l;
                        n1 = rule == 2U ? c : b;
                        n2 = rule == 2U ? b : c;
                    }
                    // current local energy: contractions at p and x
                    const crd::f64 cur = local_score(node_flops(p), node_flops(x), node_size(p), node_size(x));
                    // proposed: inner = (n0, n1), outer = (inner, n2). Compute
                    // WITHOUT mutating: sizes/flops from leg walks.
                    const crd::u64 f0 = flops_of(legs(n0), legs(n1));
                    if (!merge_into_scratch(n0, n1))
                    {
                        return HyperStatus::AllocFailed;
                    }
                    const crd::containers::ConstSpan<HyperLeg> inner{m_scratch.data(), m_scratch.size()};
                    const crd::u64 s0 = size_of(inner);
                    const crd::u64 f1 = flops_of(inner, legs(n2));
                    const crd::f64 prop = local_score(f0, f1, s0, node_size(p));
                    const crd::f64 de = prop - cur;
                    bool accept = de <= 0.0;
                    if (!accept)
                    {
                        crd::f64 u = rng.next_f64();
                        if (u <= 0.0)
                        {
                            u = 0x1p-53;
                        }
                        accept = std::log(u) < -de / temp;
                    }
                    if (accept)
                    {
                        // x becomes (n0, n1) with the merged legs; p = (x, n2)
                        crd::u32 ioff = 0;
                        crd::u32 ilen = 0;
                        if (!commit_scratch(ioff, ilen))
                        {
                            return HyperStatus::AllocFailed;
                        }
                        m_child_a[x] = n0;
                        m_child_b[x] = n1;
                        m_off[x] = ioff;
                        m_len[x] = ilen;
                        m_child_a[p] = x;
                        m_child_b[p] = n2;
                        // p's legs unchanged (same overall contraction)
                    }
                }
                // recurse into children with extent > 2 (an internal child that
                // has at least one internal child of its own)
                const crd::u32 l2 = m_child_a[p];
                const crd::u32 r2 = m_child_b[p];
                for (crd::u32 ch : {l2, r2})
                {
                    if (is_internal(ch) && (is_internal(m_child_a[ch]) || is_internal(m_child_b[ch])))
                    {
                        if (!queue.try_push_back(ch))
                        {
                            return HyperStatus::AllocFailed;
                        }
                    }
                }
            }
        }
    }
    return HyperStatus::Ok;
}

} // namespace crd::hesap::tensor
