#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/ordering/nested_dissection.hpp>

#include <utility> // std::move

// ---------------------------------------------------------------------------
// Multilevel nested-dissection SCAFFOLD (v2d). METIS paradigm, pure-C++ port:
//   coarsen (heavy-edge matching + contraction) → bisect the coarsest graph
//   (re-seeding BFS region-grow) → uncoarsen-project back to the original.
// No Fiduccia-Mattheyses refinement yet (v2e). Integer-deterministic: every
// tie-break resolves by ascending index (D(ord)-1/-3), coarse adjacency is built
// sorted (D(ord)-2/-4), coarse vertices are numbered by ascending lowest fine
// member + bisection re-seeds from the lowest-index unassigned vertex (D(ord)-7).
// ---------------------------------------------------------------------------

namespace crd::hesap::ordering
{

crd::u64 WeightedGraph::total_vertex_weight() const noexcept
{
    crd::u64 total = 0;
    for (crd::u32 v = 0; v < n; ++v)
    {
        total += vwgt[v];
    }
    return total;
}

namespace detail
{

WeightedGraph to_weighted(const AdjacencyGraph& g, crd::memory::IAllocator* alloc)
{
    WeightedGraph w(alloc);
    w.n = g.n;
    w.xadj.resize(g.xadj.size());
    for (crd::usize i = 0; i < g.xadj.size(); ++i)
    {
        w.xadj[i] = g.xadj[i];
    }
    const crd::u32 nedges = g.xadj.empty() ? 0U : g.xadj[g.n];
    w.adjncy.resize(nedges);
    w.adjwgt.resize(nedges);
    for (crd::u32 p = 0; p < nedges; ++p)
    {
        w.adjncy[p] = g.adjncy[p];
        w.adjwgt[p] = 1U; // base level: unit edge weights
    }
    w.vwgt.resize(g.n);
    for (crd::u32 v = 0; v < g.n; ++v)
    {
        w.vwgt[v] = 1U; // base level: unit vertex weights
    }
    return w;
}

crd::u32 coarsen_match(const WeightedGraph& g, crd::containers::Array<crd::u32>& cmap, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    const crd::u32 unmatched = n; // sentinel ("not yet matched")

    crd::containers::Array<crd::u32> match(alloc);
    match.resize(n);
    for (crd::u32 v = 0; v < n; ++v)
    {
        match[v] = unmatched;
    }

    // Heavy-edge matching: ascending visit order; heaviest unmatched neighbour
    // wins, ties broken by lowest neighbour index (adjncy is ascending → the
    // first occurrence of the max weight is the lowest index, so strict `>`).
    for (crd::u32 v = 0; v < n; ++v)
    {
        if (match[v] != unmatched)
        {
            continue;
        }
        crd::u32 best_u = unmatched;
        crd::u32 best_w = 0U;
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            const crd::u32 u = g.adjncy[p];
            if (match[u] != unmatched)
            {
                continue; // neighbour already matched this round
            }
            if (g.adjwgt[p] > best_w)
            {
                best_w = g.adjwgt[p];
                best_u = u;
            }
        }
        if (best_u != unmatched)
        {
            match[v] = best_u;
            match[best_u] = v;
        }
        else
        {
            match[v] = v; // unmatchable → singleton
        }
    }

    // Number coarse vertices by ascending lowest fine member (D(ord)-7). When we
    // first reach v with cmap unassigned, its mate u = match[v] is either v (a
    // singleton) or strictly greater than v (the lower member is always seen
    // first), so the pair gets one fresh id here.
    cmap.resize(n);
    for (crd::u32 v = 0; v < n; ++v)
    {
        cmap[v] = unmatched;
    }
    crd::u32 nc = 0U;
    for (crd::u32 v = 0; v < n; ++v)
    {
        if (cmap[v] != unmatched)
        {
            continue;
        }
        const crd::u32 u = match[v];
        if (u == v)
        {
            cmap[v] = nc++;
        }
        else
        {
            CRD_ASSERT_MSG(u > v, "coarsen_match: pair lower member must be seen first (D(ord)-7)");
            cmap[v] = nc;
            cmap[u] = nc;
            ++nc;
        }
    }
    return nc;
}

WeightedGraph contract(const WeightedGraph& g, crd::containers::ConstSpan<crd::u32> cmap, crd::u32 n_coarse,
                       crd::memory::IAllocator* alloc)
{
    WeightedGraph c(alloc);
    c.n = n_coarse;

    // Coarse vertex weights = sum of constituent fine weights (conserves total).
    c.vwgt.resize(n_coarse);
    for (crd::u32 cv = 0; cv < n_coarse; ++cv)
    {
        c.vwgt[cv] = 0U;
    }
    for (crd::u32 v = 0; v < g.n; ++v)
    {
        c.vwgt[cmap[v]] += g.vwgt[v];
    }

    // Group fine vertices by coarse id (counting sort; ascending fine order
    // within each group → deterministic).
    crd::containers::Array<crd::u32> group_ptr(alloc);
    group_ptr.resize(n_coarse + 1U);
    for (crd::u32 k = 0; k <= n_coarse; ++k)
    {
        group_ptr[k] = 0U;
    }
    for (crd::u32 v = 0; v < g.n; ++v)
    {
        ++group_ptr[cmap[v] + 1U];
    }
    for (crd::u32 k = 0; k < n_coarse; ++k)
    {
        group_ptr[k + 1U] += group_ptr[k];
    }
    crd::containers::Array<crd::u32> group_items(alloc);
    group_items.resize(g.n);
    crd::containers::Array<crd::u32> pos(alloc);
    pos.resize(n_coarse);
    for (crd::u32 k = 0; k < n_coarse; ++k)
    {
        pos[k] = group_ptr[k];
    }
    for (crd::u32 v = 0; v < g.n; ++v)
    {
        group_items[pos[cmap[v]]++] = v;
    }

    // Accumulate coarse adjacency. `marker[cu] == cv` flags cu touched while
    // building cv's row (timestamp → no per-row clear); `wsum` accumulates the
    // merged edge weight; `touched` lists this row's coarse neighbours, then is
    // sorted ascending (D(ord)-2/-4).
    crd::containers::Array<crd::u32> marker(alloc);
    marker.resize(n_coarse);
    for (crd::u32 k = 0; k < n_coarse; ++k)
    {
        marker[k] = n_coarse; // sentinel != any cv
    }
    crd::containers::Array<crd::u32> wsum(alloc);
    wsum.resize(n_coarse);
    crd::containers::Array<crd::u32> touched(alloc);

    c.xadj.resize(n_coarse + 1U);
    c.xadj[0] = 0U;
    for (crd::u32 cv = 0; cv < n_coarse; ++cv)
    {
        touched.clear();
        for (crd::u32 gi = group_ptr[cv]; gi < group_ptr[cv + 1U]; ++gi)
        {
            const crd::u32 v = group_items[gi];
            for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
            {
                const crd::u32 cu = cmap[g.adjncy[p]];
                if (cu == cv)
                {
                    continue; // self-loop after contraction
                }
                if (marker[cu] != cv)
                {
                    marker[cu] = cv;
                    wsum[cu] = g.adjwgt[p];
                    touched.push_back(cu);
                }
                else
                {
                    wsum[cu] += g.adjwgt[p];
                }
            }
        }
        crd::containers::sort(touched.data(), touched.data() + touched.size()); // ascending coarse neighbours
        for (crd::u32 i = 0; i < touched.size(); ++i)
        {
            const crd::u32 cu = touched[i];
            c.adjncy.push_back(cu);
            c.adjwgt.push_back(wsum[cu]);
        }
        c.xadj[cv + 1U] = static_cast<crd::u32>(c.adjncy.size());
    }
    return c;
}

void coarsen(WeightedGraph base, crd::containers::Array<WeightedGraph>& levels,
             crd::containers::Array<crd::containers::Array<crd::u32>>& cmaps, crd::memory::IAllocator* alloc)
{
    levels.push_back(std::move(base));
    while (true)
    {
        const crd::u32 cur_n = levels[levels.size() - 1].n;
        if (cur_n <= kCoarsestMax || levels.size() >= kMaxLevels)
        {
            break;
        }
        crd::containers::Array<crd::u32> cmap(alloc);
        const crd::u32 nc = coarsen_match(levels[levels.size() - 1], cmap, alloc);
        // Stall: matching reduced the vertex count by < 10% → further coarsening
        // is unproductive; this level is the coarsest.
        if (static_cast<crd::u64>(nc) * 10U >= static_cast<crd::u64>(cur_n) * 9U)
        {
            break;
        }
        WeightedGraph coarse = contract(levels[levels.size() - 1], {cmap.data(), cmap.size()}, nc, alloc);
        cmaps.push_back(std::move(cmap));
        levels.push_back(std::move(coarse));
    }
}

crd::containers::Array<crd::u8> bisect_coarsest(const WeightedGraph& g, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    crd::containers::Array<crd::u8> part(alloc);
    part.resize(n);
    for (crd::u32 v = 0; v < n; ++v)
    {
        part[v] = 1U; // default part 1; BFS recolours the part-0 region
    }
    if (n == 0U)
    {
        return part;
    }

    const crd::u64 total = g.total_vertex_weight();
    const crd::u64 half = total / 2U;

    crd::containers::Array<crd::u32> queue(alloc);
    crd::usize head = 0;
    crd::u64 weight_0 = 0;
    crd::u32 seed_cursor = 0;

    while (weight_0 < half)
    {
        while (seed_cursor < n && part[seed_cursor] == 0U)
        {
            ++seed_cursor; // advance to the lowest-index unassigned vertex (D(ord)-3/-7)
        }
        if (seed_cursor >= n)
        {
            break; // everything already in part 0
        }
        const crd::u32 seed = seed_cursor;
        part[seed] = 0U;
        weight_0 += g.vwgt[seed];
        queue.push_back(seed);
        while (head < queue.size() && weight_0 < half)
        {
            const crd::u32 x = queue[head++];
            for (crd::u32 p = g.xadj[x]; p < g.xadj[x + 1]; ++p)
            {
                const crd::u32 nb = g.adjncy[p];
                if (part[nb] == 1U)
                {
                    part[nb] = 0U;
                    weight_0 += g.vwgt[nb];
                    queue.push_back(nb);
                    if (weight_0 >= half)
                    {
                        break;
                    }
                }
            }
        }
        // frontier empty but under target → re-seed the next component (loop)
    }

    if (weight_0 == 0U)
    {
        part[0] = 0U; // total == 0 / half == 0 edge: keep part 0 non-empty
    }
    return part;
}

crd::containers::Array<crd::u8> project_down(crd::containers::ConstSpan<crd::u8> part_coarse,
                                             crd::containers::ConstSpan<crd::u32> cmap, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> out(alloc);
    out.resize(static_cast<crd::u32>(cmap.size()));
    for (crd::u32 v = 0; v < cmap.size(); ++v)
    {
        out[v] = part_coarse[cmap[v]];
    }
    return out;
}

crd::u64 edge_cut(const WeightedGraph& g, crd::containers::ConstSpan<crd::u8> part) noexcept
{
    crd::u64 cut = 0;
    for (crd::u32 v = 0; v < g.n; ++v)
    {
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            const crd::u32 u = g.adjncy[p];
            if (u > v && part[u] != part[v])
            {
                cut += g.adjwgt[p]; // count each undirected edge once
            }
        }
    }
    return cut;
}

// ---------------------------------------------------------------------------
// Fiduccia-Mattheyses bipartition refinement. Classic FM: per pass, repeatedly
// move the highest-gain unlocked vertex (across both sides) that keeps the
// heavier side within kBalanceTol of half, locking each moved vertex; track the
// best cumulative cut reduction over the pass and roll back to it. Up to
// kFmPasses passes; stop when a pass finds no improvement. Equal-gain ties resolve
// to the lowest vertex index (D(ord)-1). Gain buckets give O(1) max extraction.
//   gain[v] = (weight of v's edges to the OTHER side) - (weight to its OWN side)
//           = the cut reduction obtained by moving v.
// ---------------------------------------------------------------------------
crd::u64 fm_refine(const WeightedGraph& g, crd::containers::Array<crd::u8>& part, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    if (n < 2U)
    {
        return edge_cut(g, {part.data(), part.size()});
    }
    const crd::u64 total = g.total_vertex_weight();
    const crd::u64 cap = static_cast<crd::u64>(kBalanceTol * (static_cast<double>(total) * 0.5)) + 1U;

    // Gain range [-maxdeg, +maxdeg]; bucket index = gain + maxdeg.
    crd::u32 maxdeg = 0;
    for (crd::u32 v = 0; v < n; ++v)
    {
        crd::u32 d = 0;
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            d += g.adjwgt[p];
        }
        if (d > maxdeg)
        {
            maxdeg = d;
        }
    }
    const crd::i32 offset = static_cast<crd::i32>(maxdeg);
    const crd::u32 nbuckets = 2U * maxdeg + 1U;

    crd::containers::Array<crd::i32> gain(alloc);
    gain.resize(n);
    crd::containers::Array<crd::u8> locked(alloc);
    locked.resize(n);
    crd::containers::Array<crd::u32> bpos(alloc); // position of v within its bucket
    bpos.resize(n);
    // Two sides x nbuckets lists of vertex ids (swap-remove for O(1) deletion).
    crd::containers::Array<crd::containers::Array<crd::u32>> bucket0(alloc);
    crd::containers::Array<crd::containers::Array<crd::u32>> bucket1(alloc);
    bucket0.resize(nbuckets);
    bucket1.resize(nbuckets);
    crd::containers::Array<crd::u32> movelog(alloc);

    const auto gain_of = [&](crd::u32 v) -> crd::i32
    {
        crd::i32 ext = 0;
        crd::i32 intl = 0;
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            const crd::i32 w = static_cast<crd::i32>(g.adjwgt[p]);
            if (part[g.adjncy[p]] != part[v])
            {
                ext += w;
            }
            else
            {
                intl += w;
            }
        }
        return ext - intl;
    };

    crd::u64 cur_cut = edge_cut(g, {part.data(), part.size()});

    for (crd::u32 pass = 0; pass < kFmPasses; ++pass)
    {
        for (crd::u32 b = 0; b < nbuckets; ++b)
        {
            bucket0[b].clear();
            bucket1[b].clear();
        }
        for (crd::u32 v = 0; v < n; ++v)
        {
            locked[v] = 0U;
            gain[v] = gain_of(v);
            auto& bk = (part[v] == 0U) ? bucket0[static_cast<crd::u32>(gain[v] + offset)]
                                       : bucket1[static_cast<crd::u32>(gain[v] + offset)];
            bpos[v] = static_cast<crd::u32>(bk.size());
            bk.push_back(v);
        }
        crd::i32 cur_max0 = static_cast<crd::i32>(nbuckets) - 1;
        crd::i32 cur_max1 = static_cast<crd::i32>(nbuckets) - 1;

        crd::u64 w0 = 0;
        crd::u64 w1 = 0;
        for (crd::u32 v = 0; v < n; ++v)
        {
            (part[v] == 0U ? w0 : w1) += g.vwgt[v];
        }

        const auto bucket_ref = [&](crd::u8 side, crd::i32 gbk) -> crd::containers::Array<crd::u32>&
        {
            return (side == 0U) ? bucket0[static_cast<crd::u32>(gbk)] : bucket1[static_cast<crd::u32>(gbk)];
        };

        const auto advance_max = [&](crd::u8 side)
        {
            crd::i32& cm = (side == 0U) ? cur_max0 : cur_max1;
            while (cm >= 0 && bucket_ref(side, cm).empty())
            {
                --cm;
            }
        };
        // lowest-index vertex in side's current max bucket (D(ord)-1 tie-break), or n if none
        const auto top_vertex = [&](crd::u8 side) -> crd::u32
        {
            advance_max(side);
            const crd::i32 cm = (side == 0U) ? cur_max0 : cur_max1;
            if (cm < 0)
            {
                return n;
            }
            const auto& bk = bucket_ref(side, cm);
            crd::u32 best = bk[0];
            for (crd::u32 i = 1; i < bk.size(); ++i)
            {
                if (bk[i] < best)
                {
                    best = bk[i];
                }
            }
            return best;
        };
        const auto remove_from_bucket = [&](crd::u32 v)
        {
            auto& bk = bucket_ref(part[v], gain[v] + offset);
            const crd::u32 last = bk[bk.size() - 1];
            bk[bpos[v]] = last;
            bpos[last] = bpos[v];
            bk.pop_back();
        };
        const auto insert_into_bucket = [&](crd::u32 v)
        {
            const crd::i32 gbk = gain[v] + offset;
            auto& bk = bucket_ref(part[v], gbk);
            bpos[v] = static_cast<crd::u32>(bk.size());
            bk.push_back(v);
            crd::i32& cm = (part[v] == 0U) ? cur_max0 : cur_max1;
            if (gbk > cm)
            {
                cm = gbk;
            }
        };

        movelog.clear();
        crd::i64 cumulative = 0; // running cut reduction
        crd::i64 best_reduction = 0;
        crd::u32 best_count = 0;

        for (crd::u32 step = 0; step < n; ++step)
        {
            const crd::u32 c0 = top_vertex(0U);
            const crd::u32 c1 = top_vertex(1U);
            // feasible iff the gaining side stays within cap AND the losing side
            // stays non-empty (never collapse a bipartition onto one side).
            const bool feasible0 = (c0 != n) && (w1 + g.vwgt[c0] <= cap) && (w0 > g.vwgt[c0]); // move 0 -> 1
            const bool feasible1 = (c1 != n) && (w0 + g.vwgt[c1] <= cap) && (w1 > g.vwgt[c1]); // move 1 -> 0
            if (!feasible0 && !feasible1)
            {
                break;
            }
            crd::u32 mv = n;
            if (feasible0 && !feasible1)
            {
                mv = c0;
            }
            else if (feasible1 && !feasible0)
            {
                mv = c1;
            }
            else // both feasible: higher gain wins; equal gain -> lower index (D(ord)-1)
            {
                if (gain[c0] > gain[c1] || (gain[c0] == gain[c1] && c0 <= c1))
                {
                    mv = c0;
                }
                else
                {
                    mv = c1;
                }
            }

            remove_from_bucket(mv);
            cumulative += gain[mv];
            const crd::u8 old_side = part[mv];
            part[mv] = static_cast<crd::u8>(1U - old_side);
            locked[mv] = 1U;
            (old_side == 0U ? w0 : w1) -= g.vwgt[mv];
            (old_side == 0U ? w1 : w0) += g.vwgt[mv];
            movelog.push_back(mv);

            for (crd::u32 p = g.xadj[mv]; p < g.xadj[mv + 1]; ++p)
            {
                const crd::u32 u = g.adjncy[p];
                if (locked[u] != 0U)
                {
                    continue;
                }
                const crd::i32 dw = 2 * static_cast<crd::i32>(g.adjwgt[p]);
                remove_from_bucket(u);
                // mv now sits on part[mv]; if u shares it, mv became internal to u
                gain[u] += (part[u] == part[mv]) ? -dw : dw;
                insert_into_bucket(u);
            }

            if (cumulative > best_reduction)
            {
                best_reduction = cumulative;
                best_count = step + 1U;
            }
        }

        // roll back every move after the best prefix
        for (crd::u32 k = static_cast<crd::u32>(movelog.size()); k-- > best_count;)
        {
            const crd::u32 v = movelog[k];
            part[v] = static_cast<crd::u8>(1U - part[v]);
        }

        if (best_reduction <= 0)
        {
            break; // pass found no improvement -> converged
        }
        cur_cut -= static_cast<crd::u64>(best_reduction);
    }
    return cur_cut;
}

crd::containers::Array<crd::u8> bipartition_refined(WeightedGraph base, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> part(alloc);
    if (base.n == 0U)
    {
        return part;
    }
    if (base.n == 1U)
    {
        part.resize(1);
        part[0] = 0U;
        return part;
    }
    crd::containers::Array<WeightedGraph> levels(alloc);
    crd::containers::Array<crd::containers::Array<crd::u32>> cmaps(alloc);
    coarsen(std::move(base), levels, cmaps, alloc);

    part = bisect_coarsest(levels[levels.size() - 1], alloc);
    fm_refine(levels[levels.size() - 1], part, alloc);
    for (crd::usize i = cmaps.size(); i-- > 0;) // uncoarsen: coarsest -> ... -> original
    {
        part = project_down({part.data(), part.size()}, {cmaps[i].data(), cmaps[i].size()}, alloc);
        fm_refine(levels[i], part, alloc); // refine at every level (METIS-style)
    }
    return part;
}

namespace
{
// Kuhn augmenting-path step for bipartite matching (ascending neighbour order →
// deterministic). `lptr`/`ladj` is the left-vertex CSR adjacency (right ids);
// match_r[r] = matched left or -1; visited marks right vertices this search.
bool kuhn_augment(crd::u32 l, const crd::u32* lptr, const crd::u32* ladj, crd::i32* match_r, crd::i32* match_l,
                  crd::u8* visited)
{
    for (crd::u32 p = lptr[l]; p < lptr[l + 1]; ++p)
    {
        const crd::u32 r = ladj[p];
        if (visited[r] != 0U)
        {
            continue;
        }
        visited[r] = 1U;
        if (match_r[r] == -1 || kuhn_augment(static_cast<crd::u32>(match_r[r]), lptr, ladj, match_r, match_l, visited))
        {
            match_r[r] = static_cast<crd::i32>(l);
            match_l[l] = static_cast<crd::i32>(r);
            return true;
        }
    }
    return false;
}
} // namespace

crd::containers::Array<crd::u32> vertex_separator(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u8> part,
                                                  crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    // Boundary vertices (>= 1 cut edge): left = part-0 boundary, right = part-1.
    crd::containers::Array<crd::i32> right_id(alloc); // original -> right-local id (-1 if not a right boundary vtx)
    right_id.resize(n);
    crd::containers::Array<crd::u32> left_verts(alloc);
    crd::containers::Array<crd::u32> right_verts(alloc);
    for (crd::u32 v = 0; v < n; ++v)
    {
        right_id[v] = -1;
        bool boundary = false;
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            if (part[g.adjncy[p]] != part[v])
            {
                boundary = true;
                break;
            }
        }
        if (boundary)
        {
            if (part[v] == 0U)
            {
                left_verts.push_back(v);
            }
            else
            {
                right_id[v] = static_cast<crd::i32>(right_verts.size());
                right_verts.push_back(v);
            }
        }
    }
    const crd::u32 num_l = static_cast<crd::u32>(left_verts.size());
    const crd::u32 num_r = static_cast<crd::u32>(right_verts.size());

    // Left-vertex CSR adjacency over cut edges (right-local ids, ascending).
    crd::containers::Array<crd::u32> lptr(alloc);
    lptr.resize(num_l + 1U);
    lptr[0] = 0U;
    crd::containers::Array<crd::u32> ladj(alloc);
    for (crd::u32 l = 0; l < num_l; ++l)
    {
        const crd::u32 v = left_verts[l];
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            const crd::u32 u = g.adjncy[p];
            if (part[u] != 0U && right_id[u] >= 0)
            {
                ladj.push_back(static_cast<crd::u32>(right_id[u])); // u ascending -> right_id[u] ascending
            }
        }
        lptr[l + 1U] = static_cast<crd::u32>(ladj.size());
    }

    // Maximum bipartite matching (Kuhn, ascending left order).
    crd::containers::Array<crd::i32> match_r(alloc);
    crd::containers::Array<crd::i32> match_l(alloc);
    match_r.resize(num_r);
    match_l.resize(num_l);
    for (crd::u32 r = 0; r < num_r; ++r)
    {
        match_r[r] = -1;
    }
    for (crd::u32 l = 0; l < num_l; ++l)
    {
        match_l[l] = -1;
    }
    crd::containers::Array<crd::u8> visited(alloc);
    visited.resize(num_r);
    for (crd::u32 l = 0; l < num_l; ++l)
    {
        for (crd::u32 r = 0; r < num_r; ++r)
        {
            visited[r] = 0U;
        }
        (void)kuhn_augment(l, lptr.data(), ladj.data(), match_r.data(), match_l.data(), visited.data());
    }

    // König: min vertex cover = {left not reachable} ∪ {right reachable}, where
    // reachability runs alternating paths from UNMATCHED left vertices (unmatched
    // edges L→R, matched edges R→L). The cover is the minimum vertex separator.
    crd::containers::Array<crd::u8> zl(alloc);
    crd::containers::Array<crd::u8> zr(alloc);
    zl.resize(num_l);
    zr.resize(num_r);
    for (crd::u32 l = 0; l < num_l; ++l)
    {
        zl[l] = 0U;
    }
    for (crd::u32 r = 0; r < num_r; ++r)
    {
        zr[r] = 0U;
    }
    crd::containers::Array<crd::u32> queue(alloc);
    for (crd::u32 l = 0; l < num_l; ++l)
    {
        if (match_l[l] == -1)
        {
            zl[l] = 1U;
            queue.push_back(l);
        }
    }
    crd::usize head = 0;
    while (head < queue.size())
    {
        const crd::u32 l = queue[head++];
        for (crd::u32 p = lptr[l]; p < lptr[l + 1U]; ++p)
        {
            const crd::u32 r = ladj[p];
            if (zr[r] != 0U)
            {
                continue;
            }
            zr[r] = 1U;
            const crd::i32 lmatch = match_r[r];
            if (lmatch != -1 && zl[static_cast<crd::u32>(lmatch)] == 0U)
            {
                zl[static_cast<crd::u32>(lmatch)] = 1U;
                queue.push_back(static_cast<crd::u32>(lmatch));
            }
        }
    }

    crd::containers::Array<crd::u32> sep(alloc);
    for (crd::u32 l = 0; l < num_l; ++l)
    {
        if (zl[l] == 0U)
        {
            sep.push_back(left_verts[l]); // left_verts ascending
        }
    }
    for (crd::u32 r = 0; r < num_r; ++r)
    {
        if (zr[r] != 0U)
        {
            sep.push_back(right_verts[r]);
        }
    }
    crd::containers::sort(sep.data(), sep.data() + sep.size()); // ascending original ids
    return sep;
}

void node_fm_refine(const AdjacencyGraph& g, crd::containers::Array<crd::u8>& loc, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    crd::containers::Array<crd::u32> deg_a(alloc); // # neighbours currently in A / B
    crd::containers::Array<crd::u32> deg_b(alloc);
    crd::containers::Array<crd::u8> locked(alloc);
    deg_a.resize(n);
    deg_b.resize(n);
    locked.resize(n);
    crd::containers::Array<crd::u32> ml_vtx(alloc); // move log: vertex + its prior loc (for rollback)
    crd::containers::Array<crd::u8> ml_old(alloc);
    const crd::u64 cap = static_cast<crd::u64>(kBalanceTol * (static_cast<double>(n) * 0.5)) + 1U;

    for (crd::u32 pass = 0; pass < kFmPasses; ++pass)
    {
        crd::u64 na = 0;
        crd::u64 nb = 0;
        crd::u64 cur_s = 0;
        for (crd::u32 v = 0; v < n; ++v)
        {
            deg_a[v] = 0;
            deg_b[v] = 0;
            locked[v] = 0U;
            if (loc[v] == 0U)
            {
                ++na;
            }
            else if (loc[v] == 1U)
            {
                ++nb;
            }
            else
            {
                ++cur_s;
            }
        }
        for (crd::u32 v = 0; v < n; ++v)
        {
            for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
            {
                const crd::u8 lu = loc[g.adjncy[p]];
                if (lu == 0U)
                {
                    ++deg_a[v];
                }
                else if (lu == 1U)
                {
                    ++deg_b[v];
                }
            }
        }

        ml_vtx.clear();
        ml_old.clear();
        crd::i64 cur = static_cast<crd::i64>(cur_s);
        crd::i64 best_s = cur; // best |S| seen; prefix length 0 = no moves applied
        crd::u32 best_len = 0;

        // Classic FM: each step take the highest-gain feasible separator move (incl.
        // uphill), lock the moved vertices, track the best |S| prefix, roll back.
        // Moving s -> A pulls every B-neighbour of s into S; gain = 1 - deg_B[s].
        while (true)
        {
            crd::i32 best_gain = -1000000000;
            crd::u32 best_v = n;
            crd::u8 best_dir = 3U;
            for (crd::u32 s = 0; s < n; ++s) // ascending -> lowest-index wins ties (D(ord)-1)
            {
                if (loc[s] != 2U || locked[s] != 0U)
                {
                    continue;
                }
                if (na + 1U <= cap)
                {
                    const crd::i32 ga = 1 - static_cast<crd::i32>(deg_b[s]); // move s -> A
                    if (ga > best_gain)
                    {
                        best_gain = ga;
                        best_v = s;
                        best_dir = 0U;
                    }
                }
                if (nb + 1U <= cap)
                {
                    const crd::i32 gb = 1 - static_cast<crd::i32>(deg_a[s]); // move s -> B
                    if (gb > best_gain)
                    {
                        best_gain = gb;
                        best_v = s;
                        best_dir = 1U;
                    }
                }
            }
            if (best_v == n)
            {
                break; // no feasible separator move
            }
            const crd::u32 s = best_v;
            const crd::u8 dir = best_dir; // s moves to this side
            const crd::u8 other = (dir == 0U) ? 1U : 0U;
            const crd::u32 pulled = (dir == 0U) ? deg_b[s] : deg_a[s]; // other-side neighbours -> S

            ml_vtx.push_back(s);
            ml_old.push_back(2U);
            locked[s] = 1U;
            loc[s] = dir;
            for (crd::u32 p = g.xadj[s]; p < g.xadj[s + 1]; ++p)
            {
                (dir == 0U ? deg_a : deg_b)[g.adjncy[p]] += 1U; // s joined `dir`
            }
            (dir == 0U ? na : nb) += 1U;
            for (crd::u32 p = g.xadj[s]; p < g.xadj[s + 1]; ++p)
            {
                const crd::u32 b = g.adjncy[p];
                if (loc[b] == other) // pull into the separator
                {
                    ml_vtx.push_back(b);
                    ml_old.push_back(other);
                    locked[b] = 1U;
                    loc[b] = 2U;
                    for (crd::u32 q = g.xadj[b]; q < g.xadj[b + 1]; ++q)
                    {
                        (other == 0U ? deg_a : deg_b)[g.adjncy[q]] -= 1U; // b left `other`
                    }
                }
            }
            (other == 0U ? na : nb) -= pulled;
            cur += static_cast<crd::i64>(pulled) - 1; // |S| change
            if (cur < best_s)
            {
                best_s = cur;
                best_len = static_cast<crd::u32>(ml_vtx.size());
            }
        }

        for (crd::u32 k = static_cast<crd::u32>(ml_vtx.size()); k-- > best_len;)
        {
            loc[ml_vtx[k]] = ml_old[k]; // roll back to the best prefix
        }
        if (best_len == 0U)
        {
            break; // pass found no improving prefix -> converged
        }
    }
}

AdjacencyGraph induced_subgraph(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> verts,
                                crd::memory::IAllocator* alloc)
{
    AdjacencyGraph s(alloc);
    const crd::u32 k = static_cast<crd::u32>(verts.size());
    s.n = k;
    crd::containers::Array<crd::i32> local(alloc); // original -> new id (-1 if dropped)
    local.resize(g.n);
    for (crd::u32 v = 0; v < g.n; ++v)
    {
        local[v] = -1;
    }
    for (crd::u32 i = 0; i < k; ++i)
    {
        local[verts[i]] = static_cast<crd::i32>(i); // verts ascending -> local monotonic in original id
    }
    s.xadj.resize(k + 1U);
    s.xadj[0] = 0U;
    for (crd::u32 i = 0; i < k; ++i)
    {
        const crd::u32 v = verts[i];
        for (crd::u32 p = g.xadj[v]; p < g.xadj[v + 1]; ++p)
        {
            const crd::i32 nu = local[g.adjncy[p]];
            if (nu >= 0)
            {
                s.adjncy.push_back(static_cast<crd::u32>(nu)); // ascending preserved (g.adjncy ascending)
            }
        }
        s.xadj[i + 1U] = static_cast<crd::u32>(s.adjncy.size());
    }
    return s;
}

} // namespace detail

crd::containers::Array<crd::u8> nd_bipartition(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    const AdjacencyGraph g = build_adjacency(pattern, alloc);
    crd::containers::Array<crd::u8> part(alloc);
    if (g.n == 0U)
    {
        return part;
    }
    if (g.n == 1U)
    {
        part.resize(1);
        part[0] = 0U;
        return part;
    }
    // Single path: the public bipartition IS the FM-refined multilevel one.
    return detail::bipartition_refined(detail::to_weighted(g, alloc), alloc);
}

namespace
{
// Assign each vertex a constraint class = its rank in a POSTORDER of the
// separator tree (CHOLMOD `cmember`). Recursive bisection: recurse(A) gets the
// lowest classes, recurse(B) the next, and THIS separator the next class above
// both -> a separator is always a higher class than the interior it borders, so
// the later CAMD eliminates it after that interior. `cmember_out` is indexed by
// ORIGINAL vertex id; `label[i]` is the global id of g's local vertex i.
void assign_cmember(const AdjacencyGraph& g, crd::containers::ConstSpan<crd::u32> label,
                    crd::containers::Array<crd::u32>& cmember_out, crd::u32& next_class, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = g.n;
    if (n == 0U)
    {
        return;
    }
    const auto assign_all = [&]() // one box -> one class
    {
        const crd::u32 c = next_class++;
        for (crd::u32 v = 0; v < n; ++v)
        {
            cmember_out[label[v]] = c;
        }
    };
    if (n <= detail::kAmdThreshold)
    {
        assign_all();
        return;
    }

    auto part = detail::bipartition_refined(detail::to_weighted(g, alloc), alloc);
    auto sep = detail::vertex_separator(g, {part.data(), part.size()}, alloc);

    crd::containers::Array<crd::u8> loc(alloc); // 0=A, 1=B, 2=separator
    loc.resize(n);
    for (crd::u32 v = 0; v < n; ++v)
    {
        loc[v] = part[v];
    }
    for (crd::u32 i = 0; i < sep.size(); ++i)
    {
        loc[sep[i]] = 2U;
    }
    detail::node_fm_refine(g, loc, alloc);

    crd::containers::Array<crd::u32> a_verts(alloc);
    crd::containers::Array<crd::u32> b_verts(alloc);
    crd::containers::Array<crd::u32> sep_verts(alloc); // refined separator, ascending
    for (crd::u32 v = 0; v < n; ++v)
    {
        if (loc[v] == 2U)
        {
            sep_verts.push_back(v);
        }
        else
        {
            (loc[v] == 0U ? a_verts : b_verts).push_back(v);
        }
    }
    if (a_verts.empty() || b_verts.empty()) // bisection failed to split -> one box
    {
        assign_all();
        return;
    }

    const AdjacencyGraph ga = detail::induced_subgraph(g, {a_verts.data(), a_verts.size()}, alloc);
    const AdjacencyGraph gb = detail::induced_subgraph(g, {b_verts.data(), b_verts.size()}, alloc);
    crd::containers::Array<crd::u32> label_a(alloc);
    crd::containers::Array<crd::u32> label_b(alloc);
    label_a.resize(static_cast<crd::u32>(a_verts.size()));
    label_b.resize(static_cast<crd::u32>(b_verts.size()));
    for (crd::u32 i = 0; i < a_verts.size(); ++i)
    {
        label_a[i] = label[a_verts[i]];
    }
    for (crd::u32 i = 0; i < b_verts.size(); ++i)
    {
        label_b[i] = label[b_verts[i]];
    }

    assign_cmember(ga, {label_a.data(), label_a.size()}, cmember_out, next_class, alloc);
    assign_cmember(gb, {label_b.data(), label_b.size()}, cmember_out, next_class, alloc);
    const crd::u32 sep_class = next_class++; // separator class is ABOVE both halves
    for (crd::u32 i = 0; i < sep_verts.size(); ++i)
    {
        cmember_out[label[sep_verts[i]]] = sep_class;
    }
}
} // namespace

Permutation nd_order(const AdjacencyGraph& graph, crd::memory::IAllocator* alloc)
{
    if (graph.n == 0U)
    {
        Permutation empty(alloc);
        empty.rebuild_inverse();
        return empty;
    }
    // 1) separator-tree postorder class per vertex; 2) constrained AMD on the FULL
    //    graph (interface-aware min-degree within each class).
    crd::containers::Array<crd::u32> cmember(alloc);
    crd::containers::Array<crd::u32> ident(alloc);
    cmember.resize(graph.n);
    ident.resize(graph.n);
    for (crd::u32 v = 0; v < graph.n; ++v)
    {
        cmember[v] = 0U;
        ident[v] = v;
    }
    crd::u32 next_class = 0;
    assign_cmember(graph, {ident.data(), ident.size()}, cmember, next_class, alloc);
    return detail::camd_order(graph, {cmember.data(), cmember.size()}, alloc);
}

Permutation nd_order(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc)
{
    return nd_order(build_adjacency(pattern, alloc), alloc);
}

} // namespace crd::hesap::ordering
