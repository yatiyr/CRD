#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/direct/lu_symbolic.hpp>
#include <crd/hesap/ordering/symbolic.hpp>

#include <utility>

namespace crd::hesap::direct
{
namespace
{
// Relaxed-supernode zero budget (CHOLMOD/Cholesky D(direct)-2 default; bench-tunable). 0 = exact
// nesting (no amalgamation → no BLAS-3); larger fattens panels (more explicit-zero flops).
constexpr crd::u32 kLuSupernodeRelax = 8;

// Max amalgamated supernode (= active panel) width. Eigen SparseLU bounds its ACTIVE panel to
// panel_size=16 (decoupled from maxsuper=128 storage) to keep the dense work array cache-resident;
// our supernode IS the active panel (W[pnr × nc]), so capping width bounds the work-array footprint
// (the af23560 cache-cold-scatter / GEMM-30-not-51 floor). ADAPTIVE on the panel's L-column height:
// BLAS-3 amortization of the foot GEMM scales with foot rows, so TALL structured panels (CFD/FEM —
// af23560/wang3) get the wide cap (better GEMM/fewer updates) while SHORT circuit panels get the
// narrow cap (work array stays cache-resident). Bench-tuned on the unsymmetric corpus.
constexpr crd::u32 kLuPanelWide = 64;       // tall panels: wider GEMM N amortizes the L-foot read
constexpr crd::u32 kLuPanelNarrow = 32;     // short panels: keep W[pnr×nc] cache-resident
constexpr crd::u32 kLuPanelTallRows = 64;  // L-column height at/above which the wide cap pays off
[[nodiscard]] inline crd::u32 lu_panel_wcap(crd::u32 lead_col_height) noexcept
{
    return lead_col_height >= kLuPanelTallRows ? kLuPanelWide : kLuPanelNarrow;
}

// Pattern-only Gilbert-Peierls reachability with Eisenstat-Liu SYMMETRIC PRUNING — the symbolic
// twin of sparse_lu.cpp's `lu_dfs`. Iterative DFS on the PARTIAL L's directed graph: `pinv[j]` =
// the pivot step (= L column) at which original row j was eliminated, or < 0 if not yet (a source).
// Pushes every visited node onto xi[--top] in topological order; the unit diagonal (first entry of
// each L column) is skipped via lp[col]+1. `lpend[c]` is the PRUNED end of L column c's scan range
// (set by `prune` once a symmetric nonzero pair is found) — traversing only [lp[c]+1, lpend[c])
// instead of the full column turns the reachability from O(flops) into O(fill) (KLU/SuperLU). The
// transitive closure is unchanged (Eisenstat-Liu), so the emitted structure is identical.
[[nodiscard]] crd::u32 lu_dfs_pattern(crd::u32 jstart, const crd::u32* lp, const crd::u32* lpend, const crd::u32* li,
                                      const crd::i32* pinv, crd::u32 top, crd::u32* xi, crd::u32* pstack,
                                      crd::u8* marked) noexcept
{
    crd::i32 head = 0;
    xi[0] = jstart;
    while (head >= 0)
    {
        const crd::u32 j = xi[static_cast<crd::u32>(head)];
        const crd::i32 jnew = pinv[j]; // L column for node j (<0 ⇒ unpivoted source)
        if (marked[j] == 0)
        {
            marked[j] = 1;
            pstack[head] = (jnew < 0) ? 0U : (lp[static_cast<crd::u32>(jnew)] + 1U); // +1 skips the unit diagonal
        }
        bool done = true;
        const crd::u32 pend = (jnew < 0) ? 0U : lpend[static_cast<crd::u32>(jnew)]; // pruned scan end
        for (crd::u32 p = pstack[head]; p < pend; ++p)
        {
            const crd::u32 i = li[p]; // neighbour (original row)
            if (marked[i] != 0)
            {
                continue;
            }
            pstack[head] = p; // pause node j here
            xi[++head] = i;   // descend into i
            done = false;
            break;
        }
        if (done)
        {
            --head;
            xi[--top] = j; // node j finished → output (topological order)
        }
    }
    return top;
}

// Supernode partition over columns by EXACT structural nesting (SuperLU fundamental supernodes).
// Column j (≥1) joins j-1's supernode iff L(:,j) is EXACTLY L(:,j-1) with the diagonal j-1
// removed — i.e. j-1's sub-diagonal row list equals j's full row list (same count AND same rows).
//
// Why not the Cholesky colcount-nesting proxy (colcount[j-1]==colcount[j]+1 + etree chain): that
// equivalence is the Liu-Ng-Peyton theorem for the SYMMETRIC Cholesky factor; for the UNSYMMETRIC
// LU L factor equal column counts do NOT imply equal structure, so the proxy over-merges columns
// whose patterns differ (a member column then carries a row the leading column lacks → the dense
// panel is not a trapezoid → out-of-bounds in the numeric). The exact row-list comparison is O(nnz(L))
// total and guarantees the leading column's pattern ⊇ every member's ⇒ a true dense trapezoidal panel.
void detect_supernodes(LuSymbolic& s)
{
    const crd::u32 n = s.n;
    s.super.clear();
    s.super.push_back(0U);
    crd::u32 cur_start = 0;                                       // start column of the in-progress supernode
    crd::u32 wcap = lu_panel_wcap(s.lp[1] - s.lp[0]);             // adaptive width cap for the current panel
    for (crd::u32 j = 1; j < n; ++j)
    {
        const crd::u32 cnt_prev = s.lp[j] - s.lp[j - 1]; // colcount(j-1)
        const crd::u32 cnt_j = s.lp[j + 1] - s.lp[j];    // colcount(j)
        bool chain = (cnt_prev == cnt_j + 1);            // j-1 has exactly one more entry (its diagonal)
        for (crd::u32 t = 0; chain && t < cnt_j; ++t)    // and the remaining rows match exactly
        {
            if (s.li[s.lp[j - 1] + 1 + t] != s.li[s.lp[j] + t])
            {
                chain = false;
            }
        }
        // Cap the (active-panel) width even for EXACT supernodes: structurally-symmetric matrices
        // (CFD/FEM) form wide exact nests whose W[pnr×nc] work array goes cache-cold. Splitting an
        // exact nest is free (no extra fill — the pieces are genuinely nested + update via cmod).
        if (!chain || (j - cur_start) >= wcap)
        {
            s.super.push_back(j);
            cur_start = j;
            wcap = lu_panel_wcap(s.lp[j + 1] - s.lp[j]); // re-evaluate for the new panel's lead column
        }
    }
    s.nsuper = static_cast<crd::u32>(s.super.size());
    s.super.push_back(n);
}

// Sorted set union of `a` (ascending) and b[0..bn) (ascending) into `dst`.
void union_into(const crd::containers::Array<crd::u32>& a, const crd::u32* b, crd::u32 bn,
                crd::containers::Array<crd::u32>& dst)
{
    dst.clear();
    crd::u32 i = 0;
    crd::u32 j = 0;
    const crd::u32 an = static_cast<crd::u32>(a.size());
    while (i < an && j < bn)
    {
        if (a[i] < b[j])
        {
            dst.push_back(a[i++]);
        }
        else if (a[i] > b[j])
        {
            dst.push_back(b[j++]);
        }
        else
        {
            dst.push_back(a[i++]);
            ++j;
        }
    }
    while (i < an)
    {
        dst.push_back(a[i++]);
    }
    while (j < bn)
    {
        dst.push_back(b[j++]);
    }
}

// RELAXED amalgamation of the L supernodes (the BLAS-3 crush lever, D(direct)-2). The exact
// (structural-nesting) supernodes are size-1 on unsymmetric matrices ⇒ no dense panels ⇒ no
// BLAS-3. Merge consecutive exact supernodes along the COLUMN-ETREE chain when the merged panel's
// extra explicit zeros ≤ nrelax·(merged cols); the merged supernode's row pattern is the genuine
// UNION of its members'. L is rebuilt with each column padded to the union suffix (explicit zeros
// factor as zeros ⇒ a CORRECT factor; the union panel is a dense trapezoid for the cmod GEMM). U is
// kept EXACT (the padding L zeros contribute 0 to U, so the U structure/values are unchanged).
void relax_amalgamate_l(LuSymbolic& s, crd::u32 nrelax, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = s.n;
    const crd::u32 nf = s.nsuper;
    if (n == 0 || nf == 0)
    {
        return;
    }
    crd::containers::Array<crd::u32> col_fs(alloc); // column → exact supernode
    col_fs.resize(n);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 c = s.super[f]; c < s.super[f + 1]; ++c)
        {
            col_fs[c] = f;
        }
    }
    const auto parent_fs = [&](crd::u32 f) -> crd::u32
    {
        const crd::u32 last = s.super[f + 1] - 1;
        const crd::u32 p = s.col_etree[last];
        return (p == ordering::kNoParent) ? ordering::kNoParent : col_fs[p];
    };
    const auto colcount = [&](crd::u32 c) -> crd::u64
    {
        return s.lp[c + 1] - s.lp[c];
    };

    crd::containers::Array<crd::u32> new_super(alloc);
    crd::containers::Array<crd::u32> new_lp(alloc);
    crd::containers::Array<crd::u32> new_li(alloc);
    new_super.push_back(0U);
    new_lp.push_back(0U);
    new_li.reserve(s.li.size());
    crd::containers::Array<crd::u32> merged(alloc);
    crd::containers::Array<crd::u32> cand(alloc);

    crd::u32 f = 0;
    while (f < nf)
    {
        const crd::u32 g = f;
        const crd::u32 gc0 = s.super[g];
        merged.clear();
        for (crd::u32 p = s.lp[gc0]; p < s.lp[gc0 + 1]; ++p) // g's leading L pattern (ascending, diagonal incl.)
        {
            merged.push_back(s.li[p]);
        }
        crd::u64 c_cols = s.super[g + 1] - gc0;
        crd::u64 fund_struct = 0;
        for (crd::u32 c = gc0; c < s.super[g + 1]; ++c)
        {
            fund_struct += colcount(c);
        }
        crd::u32 cur = g;
        while (cur + 1 < nf && parent_fs(cur) == cur + 1) // chain along the column etree
        {
            const crd::u32 nxt = cur + 1;
            const crd::u32 nc0 = s.super[nxt];
            union_into(merged, &s.li[s.lp[nc0]], s.lp[nc0 + 1] - s.lp[nc0], cand);
            const crd::u64 cp = c_cols + (s.super[nxt + 1] - nc0);
            const crd::u64 pp = cand.size();
            if (cp > lu_panel_wcap(static_cast<crd::u32>(pp))) // adaptive active-panel width cap (cache vs BLAS-3)
            {
                break;
            }
            crd::u64 fund_nxt = 0;
            for (crd::u32 c = nc0; c < s.super[nxt + 1]; ++c)
            {
                fund_nxt += colcount(c);
            }
            const crd::u64 trap = cp * pp - cp * (cp - 1) / 2; // merged lower-trapezoid storage
            const crd::u64 extra = trap - (fund_struct + fund_nxt);
            // HEIGHT-GATED padding budget (the honest fill lever): explicit-zero padding only pays off
            // when the panel is TALL enough for the foot GEMM to amortize it. SHORT (circuit/irregular)
            // panels get eff_relax=0 ⇒ merge only on exact nesting (zero padding) ⇒ Eigen's flop count,
            // not 1.5–1.8× more. The padding-zero excess fill was the per-core gap (add32 0.65→0.89).
            const crd::u64 eff_relax = (pp >= kLuPanelTallRows) ? static_cast<crd::u64>(nrelax) : 0;
            if (extra > eff_relax * cp)
            {
                break;
            }
            std::swap(merged, cand); // accept: merged := union (O(1) buffer swap, not an O(width) copy)
            c_cols = cp;
            fund_struct += fund_nxt;
            cur = nxt;
        }
        // Relaxed supernode [rc0, rc1): column j gets the union suffix {merged entries ≥ j}.
        // merged's first (rc1-rc0) entries are exactly [rc0..rc1), so column j starts at merged[j-rc0].
        const crd::u32 rc0 = gc0;
        const crd::u32 rc1 = s.super[cur + 1];
        const crd::u32 msz = static_cast<crd::u32>(merged.size());
        for (crd::u32 j = rc0; j < rc1; ++j)
        {
            for (crd::u32 t = j - rc0; t < msz; ++t)
            {
                new_li.push_back(merged[t]);
            }
            new_lp.push_back(static_cast<crd::u32>(new_li.size()));
        }
        new_super.push_back(rc1);
        f = cur + 1;
    }

    s.lp = std::move(new_lp);
    s.li = std::move(new_li);
    s.super = std::move(new_super);
    s.nsuper = static_cast<crd::u32>(s.super.size()) - 1;
    s.lnz = s.li.size();
}
} // namespace

LuSymbolic lu_symbolic(const sparse::SparsePattern& b, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(b.is_compressed(), "lu_symbolic requires a compressed CSC pattern");
    CRD_ASSERT_MSG(b.format == sparse::SparseFormat::Csc, "lu_symbolic requires CSC");
    CRD_ASSERT_MSG(b.rows == b.cols, "lu_symbolic requires a square matrix");

    LuSymbolic out(alloc);
    const crd::u32 n = b.cols;
    out.n = n;
    if (n == 0)
    {
        out.lp.push_back(0U);
        out.up.push_back(0U);
        out.super.push_back(0U);
        return out;
    }

    // Column etree + postorder + the Gilbert-Ng fill bound (all from crd-hesap-ordering).
    out.col_etree = ordering::column_elimination_tree(b, alloc);
    out.col_post = ordering::postorder({out.col_etree.data(), out.col_etree.size()}, alloc);
    auto cnt = ordering::column_counts_ata(b, {out.col_etree.data(), out.col_etree.size()}, alloc);
    crd::u64 bound = 0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        bound += cnt[j];
    }
    out.fill_bound = bound;

    const crd::u32* bp = b.outer_ptr.data();
    const crd::u32* bi = b.inner_idx.data();

    // nnz(L) and nnz(U) are each ≤ fill_bound (Gilbert-Ng) ⇒ pre-reserve, no growth.
    out.lp.resize(static_cast<crd::usize>(n) + 1);
    out.up.resize(static_cast<crd::usize>(n) + 1);
    out.li.reserve(bound + n);
    out.ui.reserve(bound + n);

    crd::containers::Array<crd::i32> pinv(alloc); // pivot step of each original row, or -1
    pinv.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        pinv[i] = -1;
    }
    crd::containers::Array<crd::u32> xi(alloc); // [0..n) dfs stack/output + [n..2n) pstack
    xi.resize(static_cast<crd::usize>(n) * 2);
    crd::containers::Array<crd::u8> marked(alloc);
    marked.resize(n);                             // value-init 0
    crd::containers::Array<crd::u32> urow(alloc); // U off-diagonal rows for the current column
    crd::containers::Array<crd::u32> lrow(alloc); // L sub-diagonal rows for the current column
    urow.reserve(n);
    lrow.reserve(n);
    crd::containers::Array<crd::u32> lpend(alloc); // Eisenstat-Liu pruned end of each L column's DFS scan
    lpend.resize(n);

    for (crd::u32 k = 0; k < n; ++k)
    {
        out.lp[k] = static_cast<crd::u32>(out.li.size());
        out.up[k] = static_cast<crd::u32>(out.ui.size());
        // Reachable pattern of column k over the partial L (cols 0..k-1), with symmetric pruning.
        crd::u32 top = n;
        for (crd::u32 p = bp[k]; p < bp[k + 1]; ++p)
        {
            const crd::u32 i = bi[p];
            if (marked[i] == 0)
            {
                top = lu_dfs_pattern(i, out.lp.data(), lpend.data(), out.li.data(), pinv.data(), top, xi.data(),
                                     xi.data() + n, marked.data());
            }
        }
        // Classify reachable nodes (and restore marks). pinv[i] ≥ 0 ⇒ row i (< k) is a U entry;
        // i ≠ k with pinv[i] < 0 ⇒ row i (> k) is an L entry; i == k is the diagonal (explicit).
        urow.clear();
        lrow.clear();
        for (crd::u32 px = top; px < n; ++px)
        {
            const crd::u32 i = xi[px];
            marked[i] = 0;
            if (pinv[i] >= 0)
            {
                urow.push_back(i);
            }
            else if (i != k)
            {
                lrow.push_back(i);
            }
        }
        // U(:,k): off-diagonal rows (all < k) ascending, then the diagonal U(k,k) last.
        crd::containers::sort(urow.data(), urow.data() + urow.size());
        for (crd::u32 t = 0; t < urow.size(); ++t)
        {
            out.ui.push_back(urow[t]);
        }
        out.ui.push_back(k);
        // The static pivot is the diagonal: row k is eliminated at step k.
        pinv[k] = static_cast<crd::i32>(k);
        // L(:,k): unit diagonal L(k,k) first, then sub-diagonal rows in REACH order (NOT sorted yet —
        // the prune below partitions in place + the DFS reads it via lpend; a final pass sorts them).
        out.li.push_back(k);
        for (crd::u32 t = 0; t < lrow.size(); ++t)
        {
            out.li.push_back(lrow[t]);
        }
        lpend[k] = static_cast<crd::u32>(out.li.size()); // column end (== out.lp[k+1]); unpruned

        // SYMMETRIC PRUNING (Eisenstat-Liu / KLU): for each j with a symmetric pair — j ∈ U(:,k)
        // AND the pivot row k ∈ L(:,j) — prune column j of L by partitioning its sub-diagonal into
        // already-pivotal rows (head, the part the DFS still needs) and the rest (tail), recording
        // lpend[j] = head end. Each column is pruned at most once.
        const crd::i32* pinv_p = pinv.data();
        for (crd::u32 ti = 0; ti < urow.size(); ++ti)
        {
            const crd::u32 j = urow[ti]; // j < k, a U(:,k) entry
            if (lpend[j] != out.lp[j + 1])
            {
                continue; // already pruned
            }
            bool found = false;
            for (crd::u32 p = out.lp[j] + 1; p < out.lp[j + 1]; ++p)
            {
                if (out.li[p] == k)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                continue; // no symmetric pair
            }
            crd::u32 ph = out.lp[j] + 1;
            crd::u32 pt = out.lp[j + 1];
            while (ph < pt)
            {
                const crd::u32 r = out.li[ph];
                if (pinv_p[r] >= 0) // pivotal row → keep at head
                {
                    ++ph;
                }
                else // non-pivotal → swap to tail
                {
                    --pt;
                    out.li[ph] = out.li[pt];
                    out.li[pt] = r;
                }
            }
            lpend[j] = ph; // DFS now scans only [lp[j]+1, lpend[j]) — the pivotal head
        }
    }
    out.lp[n] = static_cast<crd::u32>(out.li.size());
    out.up[n] = static_cast<crd::u32>(out.ui.size());
    out.lnz = out.li.size();
    out.unz = out.ui.size();

    // Sort each L column's sub-diagonal ascending (the prune left it in partition order). The diagonal
    // stays first (smallest); the output is now bit-identical to the unpruned symbolic.
    for (crd::u32 j = 0; j < n; ++j)
    {
        crd::containers::sort(out.li.data() + out.lp[j] + 1, out.li.data() + out.lp[j + 1]);
    }

    detect_supernodes(out);                            // exact (structural-nesting) supernodes
    relax_amalgamate_l(out, kLuSupernodeRelax, alloc); // → relaxed BLAS-3 panels (the crush lever)
    return out;
}

} // namespace crd::hesap::direct
