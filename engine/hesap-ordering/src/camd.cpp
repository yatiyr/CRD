#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/ordering/nested_dissection.hpp>

#include <utility>

// ---------------------------------------------------------------------------
// Constrained AMD (CAMD, CHOLMOD-style) — a constraint-aware copy of the cs_amd
// port in amd.cpp. Identical quotient-graph machinery (approximate external
// degree, supervariables, mass elimination, aggressive absorption) with three
// changes that make the elimination respect a per-vertex `cmember` class:
//
//   1. SELECTION: the pivot is the lowest-degree principal whose cmember equals
//      the current (lowest non-empty) class. Classes are eliminated in ascending
//      order; within a class, approximate minimum degree (ties: lowest index).
//   2. MASS ELIMINATION + SUPERVARIABLE MERGE are gated by cmember equality — two
//      vertices in different classes must NOT be folded together (they eliminate
//      at different times).
//   3. DENSE-NODE handling is DISABLED (ordering dense nodes last would violate
//      the class order); all vertices participate normally.
//
// Used by nested dissection: cmember = separator-tree postorder rank, so a
// separator's vertices (higher class) are eliminated after the interior they
// border. The min-degree runs on the FULL graph, so it is interface-aware — the
// fix for the fill that plain per-subdomain AMD leaks into live separators.
// ---------------------------------------------------------------------------

namespace crd::hesap::ordering::detail
{
Permutation camd_order(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> cmember,
                       crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    Permutation out(alloc);
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
    crd::containers::Array<crd::u8> elim(alloc);
    crd::containers::Array<crd::u32> mark(alloc);
    crd::containers::Array<crd::u8> absorbed(alloc);
    crd::containers::Array<crd::i32> esize(alloc);
    crd::containers::Array<crd::i32> w(alloc);
    crd::containers::Array<crd::u32> wstamp(alloc);
    crd::containers::Array<crd::i32> nv(alloc);
    crd::containers::Array<crd::i32> nextm(alloc);
    crd::containers::Array<crd::i32> tailm(alloc);
    crd::containers::Array<crd::i32> hash_of(alloc);
    crd::containers::Array<crd::u32> cmpmark(alloc);
    crd::containers::Array<crd::u8> eabsorbed(alloc);
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
        pe[v] = static_cast<crd::i32>(iw.size());
        elen[v] = 0;
        nv[v] = 1;
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
    auto bucket_insert = [&](crd::i32 i, crd::i32 d)
    {
        nxt[i] = head[d];
        prv[i] = -1;
        if (head[d] != -1)
        {
            prv[head[d]] = i;
        }
        head[d] = i;
    };
    auto bucket_remove = [&](crd::i32 i, crd::i32 d)
    {
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

    // Constraint bookkeeping: number of uneliminated vertices per class + the
    // current (lowest non-empty) class. No dense-node handling under constraints.
    crd::u32 num_classes = 0;
    for (crd::u32 v = 0; v < n; ++v)
    {
        if (cmember[v] + 1U > num_classes)
        {
            num_classes = cmember[v] + 1U;
        }
    }
    crd::containers::Array<crd::u32> class_remaining(alloc);
    class_remaining.resize(num_classes);
    for (crd::u32 c = 0; c < num_classes; ++c)
    {
        class_remaining[c] = 0;
    }
    for (crd::u32 v = 0; v < n; ++v)
    {
        ++class_remaining[cmember[v]];
        degree[static_cast<crd::i32>(v)] = len[v];
        bucket_insert(static_cast<crd::i32>(v), degree[static_cast<crd::i32>(v)]);
    }
    crd::u32 cur_class = 0;

    crd::u32 stamp = 0;
    crd::u32 wround = 0;
    crd::u32 cmpstamp = 0;
    crd::i32 remaining = static_cast<crd::i32>(n);
    crd::containers::Array<crd::i32> lp(alloc);
    crd::containers::Array<crd::i32> svorder(alloc);
    crd::containers::Array<crd::i32> welist(alloc);

    auto indistinguishable = [&](crd::i32 i, crd::i32 j)
    {
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
    while (out_pos < n)
    {
        // ---- CONSTRAINED selection: lowest-degree principal of the current class ----
        while (cur_class < num_classes && class_remaining[cur_class] == 0)
        {
            ++cur_class;
        }
        crd::i32 p = -1;
        for (crd::i32 d = 0; d < static_cast<crd::i32>(n) && p == -1; ++d)
        {
            for (crd::i32 v = head[d]; v != -1; v = nxt[v])
            {
                if (cmember[static_cast<crd::u32>(v)] == cur_class && (p == -1 || v < p))
                {
                    p = v; // lowest-index cur-class principal in the lowest non-empty degree bucket
                }
            }
        }
        CRD_ASSERT_MSG(p != -1, "camd_order: no principal found in the current class");
        bucket_remove(p, degree[p]);

        // ---- form Lme (principals only) ----
        const crd::i32 pp = pe[p];
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
            const crd::i32 e = iw[pp + t];
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
        crd::i32 lme_size = 0;
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            lme_size += nv[lp[t]];
        }

        elim[p] = 1;
        remaining -= nv[p];
        pe[p] = static_cast<crd::i32>(iw.size());
        len[p] = lp_sz;
        elen[p] = 0;
        esize[p] = lme_size;
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            iw.push_back(lp[t]);
        }
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i = lp[t];
            const crd::i32 ip = pe[i];
            const crd::i32 ie_n = elen[i];
            const crd::i32 iv_n = len[i] - elen[i];
            const crd::i32 newp = static_cast<crd::i32>(iw.size());
            crd::i32 ne = 0;
            for (crd::i32 s = 0; s < ie_n; ++s)
            {
                const crd::i32 e = iw[ip + s];
                if (absorbed[e] == 0 && eabsorbed[e] == 0)
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
            pe[i] = newp;
            elen[i] = ne;
            len[i] = ne + nvar;
        }
        for (crd::i32 t = 0; t < pe_n; ++t)
        {
            absorbed[iw[pp + t]] = 0;
        }

        // ---- approximate external degree (Amestoy, nv-weighted) ----
        ++wround;
        welist.clear();
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i = lp[t];
            const crd::i32 ip = pe[i];
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
                    w[e] = esize[e];
                    welist.push_back(e);
                }
                w[e] -= nv[i];
            }
        }
        for (crd::usize k = 0; k < welist.size(); ++k)
        {
            const crd::i32 e = welist[k];
            if (w[e] == 0)
            {
                eabsorbed[e] = 1;
            }
        }
        // ---- phase A: external approximate degree + (cmember-gated) mass elimination + hash ----
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            bucket_remove(lp[t], degree[lp[t]]);
        }
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i = lp[t];
            const crd::i32 ip = pe[i];
            const crd::i32 ie_n = elen[i];
            const crd::i32 iv_n = len[i] - elen[i];
            crd::i32 dext = 0;
            crd::i64 h = 0;
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
            // Mass elimination ONLY within the same class (else i would eliminate
            // out of cmember order). Different-class d_ext==0 falls through to the
            // normal degree update and is selected when its class becomes current.
            if (dext == 0 && cmember[static_cast<crd::u32>(i)] == cmember[static_cast<crd::u32>(p)])
            {
                nextm[tailm[p]] = i;
                tailm[p] = tailm[i];
                nv[p] += nv[i];
                lme_size -= nv[i];
                remaining -= nv[i];
                nv[i] = 0;
            }
            else
            {
                if (dext < degree[i])
                {
                    degree[i] = dext;
                }
                hash_of[i] = static_cast<crd::i32>(h % static_cast<crd::i64>(n));
            }
        }
        esize[p] = lme_size;

        // emit p's supervariable chain (all members share cmember[p] by construction)
        for (crd::i32 c = p; c != -1; c = nextm[c])
        {
            out.perm[out_pos++] = static_cast<crd::u32>(c);
            --class_remaining[cmember[static_cast<crd::u32>(c)]];
        }

        // ---- phase B: supernode detection (cmember-gated indistinguishable merge) ----
        svorder.clear();
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            if (nv[lp[t]] > 0)
            {
                svorder.push_back(lp[t]);
            }
        }
        crd::containers::sort(svorder.data(), svorder.data() + svorder.size(), [&](crd::i32 a, crd::i32 b)
                              { return hash_of[a] < hash_of[b] || (hash_of[a] == hash_of[b] && a < b); });
        const crd::usize sv_n = svorder.size();
        crd::usize rb = 0;
        while (rb < sv_n)
        {
            crd::usize re = rb + 1;
            while (re < sv_n && hash_of[svorder[re]] == hash_of[svorder[rb]])
            {
                ++re;
            }
            for (crd::usize a = rb; a < re; ++a)
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
                    // merge only within the same class
                    if (cmember[static_cast<crd::u32>(i)] == cmember[static_cast<crd::u32>(j)] &&
                        indistinguishable(i, j))
                    {
                        nextm[tailm[i]] = j;
                        tailm[i] = tailm[j];
                        nv[i] += nv[j];
                        nv[j] = 0;
                    }
                }
            }
            rb = re;
        }

        // ---- phase C: finalize degree + re-insert survivors ----
        for (crd::i32 t = 0; t < lp_sz; ++t)
        {
            const crd::i32 i = lp[t];
            if (nv[i] <= 0)
            {
                continue;
            }
            crd::i32 d = degree[i] + lme_size - nv[i];
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
        }
    }
    out.rebuild_inverse();
    return out;
}
} // namespace crd::hesap::ordering::detail
