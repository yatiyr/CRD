#include <crd/hesap/ordering/mc64.hpp>

#include <crd/containers/array.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::ordering
{
namespace
{
// Min-heap of (dist, col) ordered lexicographically (dist, then ascending col) so equal-distance
// pops are deterministic (D(ord) tie-break). Lazy: a col may be pushed several times; the `done`
// flag at pop time discards stale entries.
struct HeapEntry
{
    crd::f64 dist;
    crd::u32 col;
};
[[nodiscard]] inline bool less_entry(const HeapEntry& a, const HeapEntry& b) noexcept
{
    return a.dist < b.dist || (a.dist == b.dist && a.col < b.col);
}
struct MinHeap
{
    crd::containers::Array<HeapEntry> h;
    explicit MinHeap(crd::memory::IAllocator* a) : h(a) {}
    [[nodiscard]] bool empty() const noexcept { return h.size() == 0; }
    void clear() noexcept { h.clear(); }
    void push(crd::f64 d, crd::u32 c)
    {
        h.push_back(HeapEntry{d, c});
        crd::usize i = h.size() - 1;
        while (i > 0)
        {
            const crd::usize p = (i - 1) / 2;
            if (less_entry(h[i], h[p])) { const HeapEntry t = h[i]; h[i] = h[p]; h[p] = t; i = p; }
            else { break; }
        }
    }
    HeapEntry pop()
    {
        const HeapEntry top = h[0];
        h[0]                = h[h.size() - 1];
        h.pop_back();
        crd::usize i = 0;
        const crd::usize n = h.size();
        for (;;)
        {
            const crd::usize l = 2 * i + 1;
            const crd::usize r = 2 * i + 2;
            crd::usize       s = i;
            if (l < n && less_entry(h[l], h[s])) { s = l; }
            if (r < n && less_entry(h[r], h[s])) { s = r; }
            if (s == i) { break; }
            const HeapEntry t = h[i]; h[i] = h[s]; h[s] = t; i = s;
        }
        return top;
    }
};
} // namespace

Mc64Scaling mc64_match_and_scale(const sparse::SparsePattern& pat, crd::containers::ConstSpan<crd::f64> mag,
                                 crd::memory::IAllocator* alloc)
{
    const crd::u32 n = pat.rows;
    Mc64Scaling    res(alloc);
    res.colperm.resize(n);
    res.dr.resize(n);
    res.dc.resize(n);
    if (n == 0) { return res; }

    const auto*    outer = pat.outer_ptr.data();
    const auto*    inner = pat.inner_idx.data();
    const crd::f64 inf   = std::numeric_limits<crd::f64>::infinity();

    // Per-row max magnitude (for the normalized cost log(rowmax)−log|a_ij| ≥ 0) and its log.
    crd::containers::Array<crd::f64> logrm(alloc);
    logrm.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 mx = 0.0;
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k) { if (mag[k] > mx) { mx = mag[k]; } }
        logrm[i] = mx > 0.0 ? std::log(mx) : -inf;
    }

    crd::containers::Array<crd::f64> u(alloc); // row dual potentials
    crd::containers::Array<crd::f64> v(alloc); // col dual potentials
    crd::containers::Array<crd::i32> mrow(alloc);
    crd::containers::Array<crd::i32> mcol(alloc);
    u.resize(n); v.resize(n); mrow.resize(n); mcol.resize(n);
    for (crd::u32 i = 0; i < n; ++i) { u[i] = 0.0; v[i] = 0.0; mrow[i] = -1; mcol[i] = -1; }

    // edge cost c(i, k-th nonzero) = logrm[i] − log(mag[k]) ≥ 0; +inf for a stored zero.
    auto edge_cost = [&](crd::u32 i, crd::u32 k) -> crd::f64 {
        return mag[k] > 0.0 ? (logrm[i] - std::log(mag[k])) : inf;
    };

    // ---- greedy initial matching (ascending row; each row to its smallest-cost free col) ----
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 best = inf;
        crd::i32 bj   = -1;
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::u32 j = inner[k];
            if (mcol[j] >= 0) { continue; }
            const crd::f64 c = edge_cost(i, k) - v[j];
            if (c < best || (c == best && (bj < 0 || j < static_cast<crd::u32>(bj))))
            {
                best = c; bj = static_cast<crd::i32>(j);
            }
        }
        if (bj >= 0) { mrow[i] = bj; mcol[bj] = static_cast<crd::i32>(i); u[i] = best; }
    }

    // ---- shortest-augmenting-path for each still-free row ----
    crd::containers::Array<crd::f64> dist(alloc);
    crd::containers::Array<crd::i32> predrow(alloc);
    crd::containers::Array<crd::u8>  done(alloc);
    crd::containers::Array<crd::u32> scanned(alloc); // cols settled this augmentation (to reset + dual update)
    dist.resize(n); predrow.resize(n); done.resize(n);
    for (crd::u32 i = 0; i < n; ++i) { dist[i] = inf; predrow[i] = -1; done[i] = 0; }
    MinHeap heap(alloc);

    for (crd::u32 fr = 0; fr < n; ++fr)
    {
        if (mrow[fr] >= 0) { continue; }
        scanned.clear();
        heap.clear();
        crd::i32 cur = static_cast<crd::i32>(fr);
        crd::f64 delta = 0.0;
        crd::i32 sink = -1;
        // Alternating Dijkstra: scan row `cur`, settle the nearest col, hop to its matched row.
        for (;;)
        {
            const crd::u32 r = static_cast<crd::u32>(cur);
            for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
            {
                const crd::u32 j = inner[k];
                if (done[j]) { continue; }
                const crd::f64 c = edge_cost(r, k);
                if (c == inf) { continue; }
                crd::f64 rc = c - u[r] - v[j]; // reduced cost ≥ 0
                if (rc < 0.0) { rc = 0.0; }    // guard fp drift
                const crd::f64 nd = delta + rc;
                if (nd < dist[j]) { dist[j] = nd; predrow[j] = cur; heap.push(nd, j); }
            }
            crd::i32 jm = -1;
            while (!heap.empty())
            {
                const HeapEntry e = heap.pop();
                if (done[e.col]) { continue; }
                if (e.dist > dist[e.col]) { continue; } // stale
                jm = static_cast<crd::i32>(e.col);
                break;
            }
            if (jm < 0) { break; } // no augmenting path from fr
            const crd::u32 jc = static_cast<crd::u32>(jm);
            delta   = dist[jc];
            done[jc] = 1;
            scanned.push_back(jc);
            if (mcol[jc] < 0) { sink = jm; break; } // free column ⇒ augmenting path found
            cur = mcol[jc];
        }

        if (sink >= 0)
        {
            // Dual update: u[fr] += delta; scanned cols shift by (dist−delta); their matched rows
            // shift by (delta−dist) so matched edges stay tight + reduced costs stay ≥ 0.
            u[fr] += delta;
            for (crd::usize s = 0; s < scanned.size(); ++s)
            {
                const crd::u32 j = scanned[s];
                const crd::f64 shift = delta - dist[j]; // ≥ 0
                v[j] -= shift;
                if (mcol[j] >= 0) { u[static_cast<crd::u32>(mcol[j])] += shift; }
            }
            // Augment along the alternating path.
            crd::i32 j = sink;
            while (j >= 0)
            {
                const crd::i32 i  = predrow[static_cast<crd::u32>(j)];
                const crd::i32 jn = mrow[static_cast<crd::u32>(i)];
                mrow[static_cast<crd::u32>(i)] = j;
                mcol[static_cast<crd::u32>(j)] = i;
                j = jn;
            }
        }
        // Reset per-augmentation scratch (only the settled/relaxed cols were touched via the heap;
        // reset everything touched: walk scanned + any col with finite dist. Cheap: reset all that
        // were pushed by clearing dist for settled + we conservatively reset via a scan of scanned
        // plus the heap is cleared. Reset dist for cols we set: track via scanned + relaxed. To stay
        // simple + correct, reset the full dist/done/pred for the cols we touched this round.).
        for (crd::usize s = 0; s < scanned.size(); ++s) { done[scanned[s]] = 0; }
        // dist/predrow may be set for non-settled relaxed cols too; reset by full clear (O(n) per
        // free row). Acceptable for correctness; a touched-list would trim it.
        for (crd::u32 i = 0; i < n; ++i) { dist[i] = inf; predrow[i] = -1; }
    }

    // ---- assemble: colperm (matched col per row; free rows → any remaining free col) + scaling ----
    crd::containers::Array<crd::u8> col_used(alloc);
    col_used.resize(n);
    for (crd::u32 j = 0; j < n; ++j) { col_used[j] = 0; }
    res.full_rank = true;
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (mrow[i] >= 0) { res.colperm[i] = static_cast<crd::u32>(mrow[i]); col_used[mrow[i]] = 1; }
        else { res.full_rank = false; res.colperm[i] = n; /* placeholder; fixed below */ }
    }
    if (!res.full_rank) // structurally singular: assign free rows to leftover columns (ascending)
    {
        crd::u32 nextcol = 0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (res.colperm[i] != n) { continue; }
            while (nextcol < n && col_used[nextcol]) { ++nextcol; }
            res.colperm[i] = nextcol < n ? nextcol : 0;
            if (nextcol < n) { col_used[nextcol] = 1; }
        }
    }
    // Scaling from the duals: D_r[i] = exp(u[i])/rowmax[i], D_c[j] = exp(v[j]). Matched entry → 1.
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 rm = std::exp(logrm[i]); // = rowmax (or 0 for an empty row)
        res.dr[i] = (logrm[i] > -inf && std::isfinite(u[i])) ? std::exp(u[i]) / rm : 1.0;
        if (!(res.dr[i] > 0.0) || !std::isfinite(res.dr[i])) { res.dr[i] = 1.0; }
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        res.dc[j] = std::isfinite(v[j]) ? std::exp(v[j]) : 1.0;
        if (!(res.dc[j] > 0.0) || !std::isfinite(res.dc[j])) { res.dc[j] = 1.0; }
    }
    return res;
}

} // namespace crd::hesap::ordering
