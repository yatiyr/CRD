#include <crd/hesap/ordering/amd.hpp>

#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>

#include <cmath>
#include <utility>

// -----------------------------------------------------------------------
// Quotient-graph (George-Liu) elimination machinery for AMD (v2b-1).
//
// Eliminating variable p makes it an ELEMENT whose member set Lp = the
// uneliminated variables adjacent to p (via a direct edge OR a shared element).
// |Lp| is the off-diagonal count of column p of L, so colcount[p] = |Lp| + 1
// and nnz(L) = Σ_p (|Lp| + 1). The quotient graph keeps a variable's neighbours
// as (pruned direct edges) ∪ (members of the elements it belongs to), so a
// clique formed by eliminating p costs O(|Lp|) to record (one element) instead
// of O(|Lp|²) explicit edges — and edges to variables now covered by element p
// are pruned, avoiding double-counting.
//
// This slice (v2b-1) uses an explicit Array<Array> representation: correctness
// first, validated bit-exact against the independent cs_counts nnz_l. v2b-2
// adds adaptive min-approx-degree selection + the packed-workspace + supervars.
// -----------------------------------------------------------------------

namespace crd::hesap::ordering::detail
{
crd::u64 quotient_fill(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> elim_order,
                       crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    if (n == 0)
    {
        return 0;
    }
    CRD_ASSERT_MSG(static_cast<crd::u32>(elim_order.size()) == n,
                   "quotient_fill: elim_order must be a permutation of [0,n)");

    using VecU = crd::containers::Array<crd::u32>;
    crd::containers::Array<VecU> var_adj(alloc);   // direct variable neighbours (pruned)
    crd::containers::Array<VecU> elem_adj(alloc);   // elements a variable belongs to
    crd::containers::Array<VecU> emembers(alloc);   // member variables of an element (id == eliminated var id)
    var_adj.reserve(n);
    elem_adj.reserve(n);
    emembers.reserve(n);
    for (crd::u32 v = 0; v < n; ++v)
    {
        VecU a(alloc);
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            a.push_back(g.adjncy[p]);
        }
        var_adj.push_back(std::move(a));
        elem_adj.push_back(VecU(alloc));
        emembers.push_back(VecU(alloc));
    }

    crd::containers::Array<crd::u8>  elim(alloc);
    crd::containers::Array<crd::u32> mark(alloc);  // per-variable dedup stamp
    crd::containers::Array<crd::u8>  absorbed(alloc);
    elim.resize(n);
    mark.resize(n);
    absorbed.resize(n);
    crd::u32 stamp = 0;

    VecU     lp(alloc);
    crd::u64 total = 0;

    for (crd::u32 step = 0; step < n; ++step)
    {
        const crd::u32 p = elim_order[step];
        ++stamp;
        lp.clear();
        // Mark p's elements as absorbed (their members fold into the new element p).
        for (crd::usize t = 0; t < elem_adj[p].size(); ++t)
        {
            absorbed[elem_adj[p][t]] = 1;
        }
        // Lp = uneliminated direct neighbours ∪ uneliminated members of p's elements.
        for (crd::usize t = 0; t < var_adj[p].size(); ++t)
        {
            const crd::u32 j = var_adj[p][t];
            if (elim[j] == 0 && mark[j] != stamp)
            {
                mark[j] = stamp;
                lp.push_back(j);
            }
        }
        for (crd::usize t = 0; t < elem_adj[p].size(); ++t)
        {
            const crd::u32 e = elem_adj[p][t];
            for (crd::usize s = 0; s < emembers[e].size(); ++s)
            {
                const crd::u32 j = emembers[e][s];
                if (j != p && elim[j] == 0 && mark[j] != stamp)
                {
                    mark[j] = stamp;
                    lp.push_back(j);
                }
            }
        }

        total += static_cast<crd::u64>(lp.size()) + 1;  // |Lp| off-diagonal + diagonal
        elim[p] = 1;

        emembers[p].clear();
        for (crd::usize t = 0; t < lp.size(); ++t)
        {
            emembers[p].push_back(lp[t]);
        }
        for (crd::usize t = 0; t < elem_adj[p].size(); ++t)  // absorbed elements' members subsumed by p
        {
            emembers[elem_adj[p][t]].clear();
        }

        for (crd::usize t = 0; t < lp.size(); ++t)
        {
            const crd::u32 i = lp[t];
            // Prune direct edges to eliminated vars + to Lp members (now via element p).
            VecU new_va(alloc);
            for (crd::usize s = 0; s < var_adj[i].size(); ++s)
            {
                const crd::u32 j = var_adj[i][s];
                if (elim[j] == 0 && mark[j] != stamp)
                {
                    new_va.push_back(j);
                }
            }
            var_adj[i] = std::move(new_va);
            // Drop absorbed elements, add the new element p.
            VecU new_ea(alloc);
            for (crd::usize s = 0; s < elem_adj[i].size(); ++s)
            {
                const crd::u32 e = elem_adj[i][s];
                if (absorbed[e] == 0)
                {
                    new_ea.push_back(e);
                }
            }
            new_ea.push_back(p);
            elem_adj[i] = std::move(new_ea);
        }

        for (crd::usize t = 0; t < elem_adj[p].size(); ++t)  // un-mark absorbed (before clearing p's list)
        {
            absorbed[elem_adj[p][t]] = 0;
        }
        elem_adj[p].clear();
        var_adj[p].clear();
    }
    return total;
}

// ---- Rung 1: packed-workspace (Pe/Len/Elen/Iw) elimination ----------------
// Iw holds, per node, [elements (Elen entries) | variables (Len-Elen entries)].
// For an eliminated node (now an element) the list is its member variables and
// Elen == 0. Iw is a growable Array<i32>: a rewritten/created list is appended
// at the end and Pe repointed; dead space is left behind (bounded by nnz(L);
// GC reclaims it in the rung-3 perf workspace).
crd::u64 packed_fill(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> elim_order,
                     crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    if (n == 0)
    {
        return 0;
    }
    CRD_ASSERT_MSG(static_cast<crd::u32>(elim_order.size()) == n, "packed_fill: elim_order must be a permutation");

    crd::containers::Array<crd::i32> iw(alloc);
    crd::containers::Array<crd::i32> pe(alloc);
    crd::containers::Array<crd::i32> len(alloc);
    crd::containers::Array<crd::i32> elen(alloc);
    crd::containers::Array<crd::u8>  elim(alloc);
    crd::containers::Array<crd::u32> mark(alloc);
    crd::containers::Array<crd::u8>  absorbed(alloc);
    pe.resize(n);
    len.resize(n);
    elen.resize(n);
    elim.resize(n);
    mark.resize(n);
    absorbed.resize(n);

    iw.reserve(g.adjncy.size() * 2 + n);
    for (crd::u32 v = 0; v < n; ++v)  // init: each variable's list is its direct neighbours (no elements yet)
    {
        pe[v]   = static_cast<crd::i32>(iw.size());
        elen[v] = 0;
        for (crd::u32 q = g.xadj[v]; q < g.xadj[v + 1]; ++q)
        {
            iw.push_back(static_cast<crd::i32>(g.adjncy[q]));
        }
        len[v] = static_cast<crd::i32>(g.xadj[v + 1] - g.xadj[v]);
    }

    crd::u32                         stamp = 0;
    crd::containers::Array<crd::i32> lp(alloc);
    crd::u64                         total = 0;

    for (crd::u32 step = 0; step < n; ++step)
    {
        const crd::i32 p  = static_cast<crd::i32>(elim_order[step]);
        const crd::i32 pp = pe[p];
        const crd::i32 pe_n = elen[p];          // # elements in p's list
        const crd::i32 pv_n = len[p] - elen[p];  // # variables in p's list
        ++stamp;
        lp.clear();
        for (crd::i32 t = 0; t < pe_n; ++t)  // mark p's elements absorbed
        {
            absorbed[iw[pp + t]] = 1;
        }
        for (crd::i32 t = 0; t < pv_n; ++t)  // direct variable neighbours
        {
            const crd::i32 j = iw[pp + pe_n + t];
            if (elim[j] == 0 && mark[static_cast<crd::u32>(j)] != stamp)
            {
                mark[static_cast<crd::u32>(j)] = stamp;
                lp.push_back(j);
            }
        }
        for (crd::i32 t = 0; t < pe_n; ++t)  // members of p's elements
        {
            const crd::i32 e  = iw[pp + t];
            const crd::i32 ep = pe[e];
            for (crd::i32 s = 0; s < len[e]; ++s)
            {
                const crd::i32 j = iw[ep + s];
                if (j != p && elim[j] == 0 && mark[static_cast<crd::u32>(j)] != stamp)
                {
                    mark[static_cast<crd::u32>(j)] = stamp;
                    lp.push_back(j);
                }
            }
        }

        const crd::i32 lp_sz = static_cast<crd::i32>(lp.size());
        total += static_cast<crd::u64>(lp_sz) + 1;
        elim[p] = 1;

        // p becomes an element: append its member list (Lp) at the end of Iw.
        pe[p]   = static_cast<crd::i32>(iw.size());
        len[p]  = lp_sz;
        elen[p] = 0;
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            iw.push_back(lp[t]);
        }

        // Rewrite each i in Lp: [kept elements + p] then [direct vars not in Lp, not elim].
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i   = lp[t];
            const crd::i32 ip  = pe[i];
            const crd::i32 ie_n = elen[i];
            const crd::i32 iv_n = len[i] - elen[i];
            const crd::i32 newp = static_cast<crd::i32>(iw.size());
            crd::i32       ne   = 0;
            for (crd::i32 s = 0; s < ie_n; ++s)  // kept elements (drop absorbed)
            {
                const crd::i32 e = iw[ip + s];
                if (absorbed[e] == 0)
                {
                    iw.push_back(e);
                    ++ne;
                }
            }
            iw.push_back(p);  // the new element
            ++ne;
            crd::i32 nv = 0;
            for (crd::i32 s = 0; s < iv_n; ++s)  // kept direct vars (drop elim + Lp members)
            {
                const crd::i32 j = iw[ip + ie_n + s];
                if (elim[j] == 0 && mark[static_cast<crd::u32>(j)] != stamp)
                {
                    iw.push_back(j);
                    ++nv;
                }
            }
            pe[i]   = newp;
            elen[i] = ne;
            len[i]  = ne + nv;
        }

        for (crd::i32 t = 0; t < pe_n; ++t)  // un-mark absorbed
        {
            absorbed[iw[pp + t]] = 0;
        }
    }
    return total;
}
} // namespace crd::hesap::ordering::detail

namespace crd::hesap::ordering
{
// v2b-2 RUNG 2: AMD with EXACT min-degree selection on the packed workspace.
// The elimination machinery is rung 1's; on top we maintain a doubly-linked
// degree-bucket structure, select the lowest-index variable of the lowest
// non-empty bucket (D(ord)-1), and recompute each touched variable's EXACT
// external degree after the elimination. Sanity rung: a valid ordering with
// fill in the min-degree ballpark (rung 3 swaps in the Amestoy approximate
// bound + supervariables + mass elim + aggressive absorption for the gate).
Permutation amd_order(const AdjacencyGraph& g, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    Permutation    out(alloc);
    out.perm.resize(n);
    if (n == 0)
    {
        out.rebuild_inverse();
        return out;
    }

    crd::containers::Array<crd::i32> iw(alloc);
    crd::containers::Array<crd::i32> pe(alloc);
    crd::containers::Array<crd::i32> len(alloc);
    crd::containers::Array<crd::i32> elen(alloc);
    crd::containers::Array<crd::u8>  elim(alloc);
    crd::containers::Array<crd::u32> mark(alloc);
    crd::containers::Array<crd::u8>  absorbed(alloc);
    crd::containers::Array<crd::i32> esize(alloc);   // element external size at creation (nv-weighted |Le|)
    crd::containers::Array<crd::i32> w(alloc);        // scratch: |Le \ Lme| during degree update
    crd::containers::Array<crd::u32> wstamp(alloc);   // round-stamp guarding w[]
    crd::containers::Array<crd::i32> nv(alloc);       // supervariable size (0 == merged into a principal)
    crd::containers::Array<crd::i32> nextm(alloc);    // member chain: next original var of this supervariable
    crd::containers::Array<crd::i32> tailm(alloc);    // member chain tail (for O(1) merge splice)
    crd::containers::Array<crd::i32> hash_of(alloc);  // supervariable hash this round
    crd::containers::Array<crd::u32> cmpmark(alloc);  // structural-equality compare marker
    crd::containers::Array<crd::u8>  eabsorbed(alloc);// aggressive absorption: Le subset of Lme -> element dead
    pe.resize(n);
    len.resize(n);
    elen.resize(n);
    elim.resize(n);
    mark.resize(n);
    absorbed.resize(n);
    esize.resize(n);
    w.resize(n);
    wstamp.resize(n);
    nv.resize(n);
    nextm.resize(n);
    tailm.resize(n);
    hash_of.resize(n);
    cmpmark.resize(n);
    eabsorbed.resize(n);
    iw.reserve(g.adjncy.size() * 2 + n);
    for (crd::u32 v = 0; v < n; ++v)
    {
        pe[v]    = static_cast<crd::i32>(iw.size());
        elen[v]  = 0;
        nv[v]    = 1;
        nextm[v] = -1;
        tailm[v] = static_cast<crd::i32>(v);
        cmpmark[v] = 0;
        eabsorbed[v] = 0;
        for (crd::u32 q = g.xadj[v]; q < g.xadj[v + 1]; ++q)
        {
            iw.push_back(static_cast<crd::i32>(g.adjncy[q]));
        }
        len[v] = static_cast<crd::i32>(g.xadj[v + 1] - g.xadj[v]);
    }

    // Doubly-linked degree buckets.
    crd::containers::Array<crd::i32> degree(alloc);
    crd::containers::Array<crd::i32> head(alloc);
    crd::containers::Array<crd::i32> nxt(alloc);
    crd::containers::Array<crd::i32> prv(alloc);
    degree.resize(n);
    head.resize(n);
    nxt.resize(n);
    prv.resize(n);
    for (crd::u32 d = 0; d < n; ++d)
    {
        head[d] = -1;
    }
    auto bucket_insert = [&](crd::i32 i, crd::i32 d) {
        nxt[i] = head[d];
        prv[i] = -1;
        if (head[d] != -1)
        {
            prv[head[d]] = i;
        }
        head[d] = i;
    };
    auto bucket_remove = [&](crd::i32 i, crd::i32 d) {
        if (prv[i] != -1)
        {
            nxt[prv[i]] = nxt[i];
        }
        else
        {
            head[d] = nxt[i];
        }
        if (nxt[i] != -1)
        {
            prv[nxt[i]] = prv[i];
        }
    };
    // Dense-node threshold (cs_amd): nodes with degree > dense are removed from the
    // quotient graph and ordered LAST, so the sparse structure is eliminated first.
    crd::i32 dense = static_cast<crd::i32>(10.0 * std::sqrt(static_cast<double>(n)));
    if (dense < 16)
    {
        dense = 16;
    }
    if (dense > static_cast<crd::i32>(n) - 2)
    {
        dense = static_cast<crd::i32>(n) - 2;
    }
    crd::containers::Array<crd::u8> is_dense(alloc);
    is_dense.resize(n);
    crd::u32 num_dense = 0;
    crd::i32 min_deg   = static_cast<crd::i32>(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        degree[i]   = len[i];
        is_dense[i] = 0;
        if (degree[i] > dense)
        {
            is_dense[i] = 1;  // ordered last; removed from the active graph (nv == 0)
            nv[i]       = 0;
            ++num_dense;
            continue;
        }
        bucket_insert(static_cast<crd::i32>(i), degree[i]);
        if (degree[i] < min_deg)
        {
            min_deg = degree[i];
        }
    }

    crd::u32                         stamp     = 0;
    crd::u32                         wround    = 0;
    crd::u32                         cmpstamp  = 0;
    crd::i32                         remaining = static_cast<crd::i32>(n - num_dense);  // dense nodes deferred
    crd::containers::Array<crd::i32> lp(alloc);
    crd::containers::Array<crd::i32> svorder(alloc);  // Lme members sorted by (hash, index) for supervar grouping
    crd::containers::Array<crd::i32> welist(alloc);   // elements touched this round (aggressive-absorption scan)

    auto indistinguishable = [&](crd::i32 i, crd::i32 j) {
        if (len[i] != len[j] || elen[i] != elen[j])
        {
            return false;
        }
        ++cmpstamp;
        const crd::i32 ipi = pe[i];
        for (crd::i32 s = 0; s < len[i]; ++s)
        {
            cmpmark[static_cast<crd::u32>(iw[ipi + s])] = cmpstamp;
        }
        const crd::i32 ipj = pe[j];
        for (crd::i32 s = 0; s < len[j]; ++s)
        {
            if (cmpmark[static_cast<crd::u32>(iw[ipj + s])] != cmpstamp)
            {
                return false;
            }
        }
        return true;
    };

    crd::u32 out_pos = 0;
    while (out_pos < n - num_dense)
    {
        while (min_deg < static_cast<crd::i32>(n) && head[min_deg] == -1)
        {
            ++min_deg;
        }
        crd::i32 p = -1;  // lowest-index principal in the min-degree bucket (D(ord)-1)
        for (crd::i32 v = head[min_deg]; v != -1; v = nxt[v])
        {
            if (p == -1 || v < p)
            {
                p = v;
            }
        }
        bucket_remove(p, degree[p]);

        // ---- form Lme (principals only; nv==0 entries are merged-away) ----
        const crd::i32 pp   = pe[p];
        const crd::i32 pe_n = elen[p];
        const crd::i32 pv_n = len[p] - elen[p];
        ++stamp;
        lp.clear();
        for (crd::i32 t = 0; t < pe_n; ++t)
        {
            absorbed[iw[pp + t]] = 1;
        }
        for (crd::i32 t = 0; t < pv_n; ++t)
        {
            const crd::i32 j = iw[pp + pe_n + t];
            if (elim[j] == 0 && nv[j] > 0 && mark[static_cast<crd::u32>(j)] != stamp)
            {
                mark[static_cast<crd::u32>(j)] = stamp;
                lp.push_back(j);
            }
        }
        for (crd::i32 t = 0; t < pe_n; ++t)
        {
            const crd::i32 e  = iw[pp + t];
            const crd::i32 ep = pe[e];
            for (crd::i32 s = 0; s < len[e]; ++s)
            {
                const crd::i32 j = iw[ep + s];
                if (j != p && elim[j] == 0 && nv[j] > 0 && mark[static_cast<crd::u32>(j)] != stamp)
                {
                    mark[static_cast<crd::u32>(j)] = stamp;
                    lp.push_back(j);
                }
            }
        }
        const crd::i32 lp_sz = static_cast<crd::i32>(lp.size());
        crd::i32       lme_size = 0;
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            lme_size += nv[lp[t]];
        }

        elim[p] = 1;
        remaining -= nv[p];
        pe[p]    = static_cast<crd::i32>(iw.size());
        len[p]   = lp_sz;
        elen[p]  = 0;
        esize[p] = lme_size;  // nv-weighted |Lme|
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            iw.push_back(lp[t]);
        }
        // rewrite each i in Lme: [kept elements + p] then [external principal vars]
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i    = lp[t];
            const crd::i32 ip   = pe[i];
            const crd::i32 ie_n = elen[i];
            const crd::i32 iv_n = len[i] - elen[i];
            const crd::i32 newp = static_cast<crd::i32>(iw.size());
            crd::i32       ne   = 0;
            for (crd::i32 s = 0; s < ie_n; ++s)
            {
                const crd::i32 e = iw[ip + s];
                if (absorbed[e] == 0 && eabsorbed[e] == 0)  // drop pivot-covered + aggressively-absorbed elements
                {
                    iw.push_back(e);
                    ++ne;
                }
            }
            iw.push_back(p);
            ++ne;
            crd::i32 nvar = 0;
            for (crd::i32 s = 0; s < iv_n; ++s)
            {
                const crd::i32 j = iw[ip + ie_n + s];
                if (elim[j] == 0 && nv[j] > 0 && mark[static_cast<crd::u32>(j)] != stamp)
                {
                    iw.push_back(j);
                    ++nvar;
                }
            }
            pe[i]   = newp;
            elen[i] = ne;
            len[i]  = ne + nvar;
        }
        for (crd::i32 t = 0; t < pe_n; ++t)
        {
            absorbed[iw[pp + t]] = 0;
        }

        // ---- approximate external degree (Amestoy, nv-weighted) ----
        // Scan 1: w[e] = |Le \ Lme| (nv-weighted) for each prior element e of an Lme member.
        ++wround;
        welist.clear();
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i    = lp[t];
            const crd::i32 ip   = pe[i];
            const crd::i32 ie_n = elen[i];
            for (crd::i32 s = 0; s < ie_n; ++s)
            {
                const crd::i32 e = iw[ip + s];
                if (e == p || eabsorbed[e])
                {
                    continue;
                }
                if (wstamp[static_cast<crd::u32>(e)] != wround)
                {
                    wstamp[static_cast<crd::u32>(e)] = wround;
                    w[e]                             = esize[e];
                    welist.push_back(e);
                }
                w[e] -= nv[i];
            }
        }
        // Aggressive absorption: Le subset of Lme (w[e] == 0) -> e is redundant with the new element.
        for (crd::usize k = 0; k < welist.size(); ++k)
        {
            const crd::i32 e = welist[k];
            if (w[e] == 0)
            {
                eabsorbed[e] = 1;
            }
        }
        // ---- cs_amd phase A: external approximate degree + mass elimination + hash ----
        // Pull every Lme member out of its OLD degree bucket first; survivors are
        // re-inserted in phase C, mass-eliminated/merged ones are not.
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            bucket_remove(lp[t], degree[lp[t]]);
        }
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i    = lp[t];
            const crd::i32 ip   = pe[i];
            const crd::i32 ie_n = elen[i];
            const crd::i32 iv_n = len[i] - elen[i];
            crd::i32       dext = 0;  // external degree: Σ|Le\Lme| (live elements) + Σ nv(external vars)
            crd::i64       h    = 0;
            for (crd::i32 s = 0; s < ie_n; ++s)
            {
                const crd::i32 e = iw[ip + s];
                if (e != p && !eabsorbed[e])
                {
                    dext += w[e];
                    h += e;
                }
            }
            for (crd::i32 s = 0; s < iv_n; ++s)
            {
                const crd::i32 j = iw[ip + ie_n + s];
                dext += nv[j];
                h += j;
            }
            if (dext == 0)
            {
                // mass elimination (cs_amd d==0): i is contained in Lme -> fold into pivot p,
                // adds no fill. Its members ride out on p's chain; |Lme| shrinks accordingly.
                nextm[tailm[p]] = i;
                tailm[p]        = tailm[i];
                nv[p] += nv[i];
                lme_size -= nv[i];
                remaining -= nv[i];
                nv[i] = 0;
            }
            else
            {
                if (dext < degree[i])
                {
                    degree[i] = dext;  // CS_MIN(degree[i], dext): tightest external bound seen so far
                }
                hash_of[i] = static_cast<crd::i32>(h % static_cast<crd::i64>(n));
            }
        }
        esize[p] = lme_size;  // final |Lme| after mass elimination (element size for later rounds)

        // emit p's supervariable (now including any mass-eliminated members)
        for (crd::i32 c = p; c != -1; c = nextm[c])
        {
            out.perm[out_pos++] = static_cast<crd::u32>(c);
        }

        // ---- cs_amd phase B: supernode detection (merge indistinguishable survivors, D(ord)-5) ----
        svorder.clear();
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            if (nv[lp[t]] > 0)
            {
                svorder.push_back(lp[t]);
            }
        }
        crd::containers::sort(svorder.data(), svorder.data() + svorder.size(), [&](crd::i32 a, crd::i32 b) {
            return hash_of[a] < hash_of[b] || (hash_of[a] == hash_of[b] && a < b);
        });
        const crd::usize sv_n = svorder.size();
        crd::usize       rb   = 0;
        while (rb < sv_n)
        {
            crd::usize re = rb + 1;
            while (re < sv_n && hash_of[svorder[re]] == hash_of[svorder[rb]])
            {
                ++re;
            }
            for (crd::usize a = rb; a < re; ++a)  // svorder ascending index -> principal = lowest index
            {
                const crd::i32 i = svorder[a];
                if (nv[i] == 0)
                {
                    continue;
                }
                for (crd::usize b = a + 1; b < re; ++b)
                {
                    const crd::i32 j = svorder[b];
                    if (nv[j] == 0)
                    {
                        continue;
                    }
                    if (indistinguishable(i, j))
                    {
                        nextm[tailm[i]] = j;  // merge j into principal i (no bucket op: already pulled in phase A)
                        tailm[i]        = tailm[j];
                        nv[i] += nv[j];
                        nv[j] = 0;
                    }
                }
            }
            rb = re;
        }

        // ---- cs_amd phase C: finalize degree (d = degree[i] + |Lme| - nv[i]) + re-insert survivors ----
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i = lp[t];
            if (nv[i] <= 0)
            {
                continue;  // mass-eliminated or merged away
            }
            crd::i32       d         = degree[i] + lme_size - nv[i];
            const crd::i32 bound_rem = remaining - nv[i];
            if (d > bound_rem)
            {
                d = bound_rem;
            }
            if (d < 0)
            {
                d = 0;
            }
            degree[i] = d;
            bucket_insert(i, d);
            if (d < min_deg)
            {
                min_deg = d;
            }
        }
    }
    // Dense nodes last, in ascending index order (deterministic, D(ord)-1).
    for (crd::u32 v = 0; v < n; ++v)
    {
        if (is_dense[v])
        {
            out.perm[out_pos++] = v;
        }
    }
    out.rebuild_inverse();
    return out;
}

Permutation amd_order(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    return amd_order(build_adjacency(pattern, alloc), alloc);
}
} // namespace crd::hesap::ordering
