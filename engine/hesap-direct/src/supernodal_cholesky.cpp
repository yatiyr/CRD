#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <cmath>
#include <type_traits>

#if defined(CRD_HESAP_CHOL_PROFILE) || defined(CRD_HESAP_CHOL_SCALE_PROFILE)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#endif

namespace crd::hesap::direct
{
namespace dense = crd::hesap::dense;

#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
// Per-LEVEL wall-clock split for the 8T scaling diagnosis (the advisor's "diagnose WHERE scaling is
// lost"). Times each etree level on the DISPATCHER thread only ⇒ race-free + valid at >1 worker
// (unlike the per-call CHOL_PROFILE, whose shared g_cholprof += races across threads ⇒ 1T-only).
// Splits the factor wall into: node-parallel huge-front levels (lever = within-front parallelism /
// serial-section Amdahl) vs tree-parallel healthy levels vs tree-STARVED levels (cnt<num_workers AND
// no huge front ⇒ workers idle = the tree-starvation lever). max_level_wall = the critical-path peak.
namespace
{
struct ScaleProf
{
    double sym_wall_ns = 0.0;     // symbolic_factorize + build_supernodal_symbolic (serial analyze)
    double setup_wall_ns = 0.0;   // ENTIRE pre-numeric setup: symbolic + scratch alloc + level build
    double tree_wall_ns = 0.0;    // healthy tree-parallel levels (cnt >= num_workers)
    double node_wall_ns = 0.0;    // thin + huge ⇒ node-parallel (gemm_parallel within front)
    double starved_wall_ns = 0.0; // cnt < num_workers AND no huge ⇒ tree-parallel but idle workers
    double max_level_wall_ns = 0.0;
    crd::u32 n_levels = 0;
    crd::u32 n_node_levels = 0;
    crd::u32 n_starved_levels = 0;
    crd::u32 max_level_cnt = 0;
    crd::u32 max_level_nc = 0;
    void reset() noexcept { *this = ScaleProf{}; }
};
ScaleProf g_scaleprof;
using ScaleClock = std::chrono::high_resolution_clock;
inline double scale_ns(ScaleClock::time_point a, ScaleClock::time_point b) noexcept
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(b - a).count();
}
} // namespace
#endif

#ifdef CRD_HESAP_CHOL_PROFILE
// Throwaway per-phase profiler (compile-gated, OFF by default — zero overhead in
// shipped builds). Profile at 1 worker (the per-thread-efficiency regime). Reports
// the ms split scatter/cmod/cdiv + achieved GFLOP/s, so we attack the right lever.
namespace
{
struct CholProf
{
    double scatter_ns = 0.0;
    double cmod_ns = 0.0;
    double cdiv_ns = 0.0;
    double cmod_scatter_ns = 0.0; // the rr[]-indexed scatter-subtract of U into the panel (within cmod)
    crd::u64 cmod_flops = 0;
    crd::u64 cdiv_flops = 0;
    crd::u64 cmod_calls = 0; // # of per-descendant gemm calls (avg flop/call confirms low-K)
    // Per-knc-bin flop + time (bins: <8, 8-16, 17-32, 33-64, 65-128, 129+). Discriminates
    // amalgamation (low-K bandwidth-bound) vs kernel (high-K) — where do the cmod flops live?
    static constexpr int kBins = 6;
    crd::u64 cmod_flop_bin[kBins] = {};
    double cmod_ns_bin[kBins] = {};
    static int knc_bin(crd::u32 knc) noexcept
    {
        return knc < 8 ? 0 : knc < 16 ? 1 : knc < 32 ? 2 : knc < 64 ? 3 : knc < 128 ? 4 : 5;
    }
    // For the K≥128 cmod calls (85% of the flops, the 52-GF/s wall): bin by OUTPUT WIDTH m1
    // (the gemm N dim). Small m1 ⇒ skinny-C ⇒ low A-reuse ⇒ memory-bound (kernel tweaks can't
    // fix it; the lever is widening C via amalgamation). Large m1 ⇒ genuine kernel/shape.
    static constexpr int kM1Bins = 5;
    crd::u64 cmod_hk_m1_flop[kM1Bins] = {}; // m1: <16, 16-63, 64-255, 256-1023, 1024+
    crd::u64 cmod_hk_m1_count[kM1Bins] = {};
    static int m1_bin(crd::u32 m1) noexcept { return m1 < 16 ? 0 : m1 < 64 ? 1 : m1 < 256 ? 2 : m1 < 1024 ? 3 : 4; }
    // cdiv (25 GF/s — worst phase) by panel width nc: is the time in the thin scalar panels
    // (nc ≤ 32) or the fat blocked-BLAS-3 panels? Picks the cdiv lever.
    static constexpr int kCdivBins = 5;
    crd::u64 cdiv_flop_bin[kCdivBins] = {}; // nc: ≤4, 5-32, 33-127, 128-511, 512+
    double cdiv_ns_bin[kCdivBins] = {};
    static int cdiv_bin(crd::u32 nc) noexcept { return nc <= 4 ? 0 : nc <= 32 ? 1 : nc < 128 ? 2 : nc < 512 ? 3 : 4; }
    // Within-cdiv split (v5a-4 cdiv crush): is the 38%-peak loss in the OUTER trailing (step B,
    // K=obw=256, the bulk Schur gemm) or the WITHIN-panel K=64 work (A1 scalar POTF2 + A2
    // below-solve + A3 inner trailing)? cdiv_outertrail_ns times B; the rest = cdiv_ns − that.
    double cdiv_outertrail_ns = 0.0;
    void reset() noexcept { *this = CholProf{}; }
};
CholProf g_cholprof;
using ProfClock = std::chrono::high_resolution_clock;
inline double prof_ns(ProfClock::time_point a, ProfClock::time_point b) noexcept
{
    return std::chrono::duration<double, std::nano>(b - a).count();
}
} // namespace
#endif

// Real/complex bridges for the LLᵀ (real) ↔ LLᴴ (complex Hermitian) single path.
// For real T every bridge is the identity; for Complex<R> they select the real
// part / conjugate so the SAME factor + solve body computes A = L·Lᴴ. The pivot
// of an HPD matrix is real-positive — we take its real part, sqrt it, and store
// the diagonal as a pure real (zeroing the rounding residual in im so that
// chol_conj(L[j][j]) == L[j][j] downstream).
template <typename T> [[nodiscard]] constexpr dense::RealType<T> chol_real(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return x.re;
    }
    else
    {
        return x;
    }
}

template <typename T> [[nodiscard]] constexpr T chol_from_real(dense::RealType<T> d) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return T{d, dense::RealType<T>{0}};
    }
    else
    {
        return d;
    }
}

template <typename T> [[nodiscard]] constexpr T chol_conj(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return conj(x);
    }
    else
    {
        return x;
    }
}

// cmod Schur update + backward-solve adjoint: Lᴴ for complex, Lᵀ for real.
template <typename T>
inline constexpr dense::Trans kCholAdjoint =
    dense::is_complex_v<T> ? dense::Trans::ConjTranspose : dense::Trans::Transpose;

// Blocked-cdiv OUTER panel width — the K dimension of the trailing Schur gemm (B step).
// Bigger ⇒ higher arithmetic intensity + fewer (larger) trailing-gemm calls (the v5a-4 cdiv
// crush). A NAIVE single-level sweep capped at 128: its scalar diagonal POTF2 is O(bw²·nc)
// (measured — bw=256 REGRESSED vs 128). The TWO-LEVEL panel factorization (inner kCdivInnerBlock
// granularity) removes that O(bw²) cap, so the outer block can be wide. Tuned at v5a-4.
inline constexpr crd::u32 kCdivBlock = 256;

// Blocked-cdiv INNER panel width — the granularity of the scalar diagonal POTF2 + the
// kCdivInnerBlock×kCdivInnerBlock invert inside the outer panel factorization. The ONLY scalar
// work is here, total O(nc·kCdivInnerBlock²) — independent of the outer kCdivBlock (which is the
// whole point of the two-level structure). 64 matches the dense xPOTRF panel.
inline constexpr crd::u32 kCdivInnerBlock = 64;

// Supernodes with nc ≤ this use the pure-scalar rank-1 cdiv (no invert/gemm overhead):
// they hold negligible factor flops but are numerous (bmwcra: 11813/16007 have nc 2-4),
// so the per-supernode BLAS-3 setup cost would dominate their tiny work. Fat supernodes
// (nc > this) use the blocked invert+gemm BLAS-3 path. Tuned at v5a-4 close.
inline constexpr crd::u32 kCdivScalarMin = 32;

// A panel gemm (cmod / cdiv) routes to `gemm_parallel` instead of serial `gemm` only
// when (a) the supernode is processed node-parallel (few-supernode etree level ⇒ idle
// workers) AND (b) its flop count clears this bar (else fiber-dispatch overhead > win).
// This is the within-supernode parallelism for the few huge near-root fronts that
// serialize under pure tree-parallelism (the 8-thread CHOLMOD gap). gemm_parallel is
// bit-identical to serial gemm (disjoint row-slabs) ⇒ the determinism moat holds.
inline constexpr crd::u64 kGemmParallelMinFlop = 1U << 20; // ~1M flop

// A supernode counts as a HUGE front (node-parallel candidate) when its column count
// reaches this — its panel gemms then clear kGemmParallelMinFlop so gemm_parallel engages.
// A thin etree level goes node-parallel ONLY if it holds such a front; a thin level of
// only-small supernodes stays TREE-parallel (serializing it would regress small/medium
// matrices — bcsstk25 measured 1.25×→0.57× at 8 threads under blanket node-parallel).
inline constexpr crd::u32 kNodeParallelMinCols = 512;

// v5a-5: single-RHS parallel-solve fill gate. The level-parallel single-RHS solve only pays on LARGE
// factors; on small ones the per-level dispatch overhead (parallel_for + wait + frame_reset over many
// tiny levels) dwarfs the single-vector work and REGRESSES hard (measured 8-thread vs 1-thread:
// bcsstk25 ~12x, bcsstk13 ~39x slower). Corpus: regression at <=1.5M factor nnz, benefit at >=26.6M;
// conservative cut at 24M (a per-level work gate is the future refinement to capture the mid-range).
inline constexpr crd::u64 kSolveParallelMinLnz = 24'000'000;

// Invert a bw×bw lower-triangular Cholesky diagonal block L (ColMajor, leading dim
// ldl, real-positive diagonal) into linv (bw×bw ColMajor ld=bw, lower-tri; upper
// zeroed): linv = L⁻¹. Lets the cdiv below-block solve L21 = A21·L⁻ᴴ run as a single
// BLAS-3 gemm instead of the AI≈0.33 rank-1 sweep (the v5a-4 cdiv crush). bw ≤ kCdivBlock
// so this O(bw³/6) scalar inverse is negligible; the diagonal block of an SPD factor is
// well-conditioned ⇒ inverting it is numerically safe (residual-validated at slice close).
template <typename T> inline void invert_lower_tri(const T* l, crd::u32 ldl, crd::u32 bw, T* linv) noexcept
{
    for (crd::u32 i = 0; i < bw * bw; ++i)
    {
        linv[i] = T{0};
    }
    for (crd::u32 j = 0; j < bw; ++j)
    {
        linv[static_cast<crd::usize>(j) * bw + j] = T{1} / l[static_cast<crd::usize>(j) * ldl + j];
        for (crd::u32 i = j + 1; i < bw; ++i)
        {
            T s = T{0};
            for (crd::u32 k = j; k < i; ++k)
            {
                s += l[static_cast<crd::usize>(k) * ldl + i] * linv[static_cast<crd::usize>(j) * bw + k];
            }
            linv[static_cast<crd::usize>(j) * bw + i] = -s / l[static_cast<crd::usize>(i) * ldl + i];
        }
    }
}
// =======================================================================
// v5a-1b step 1 — relaxed supernode amalgamation (D(direct)-2).
//
// Builds the amalgamated supernodal structure from a v2c SymbolicFactor.
// Fundamental supernodes (sf.super) are merged along etree chains in
// postorder: a fundamental supernode `cur` merges with the NEXT one
// (`cur+1`) iff `cur+1` is `cur`'s etree-parent supernode (so their
// columns are contiguous) AND the explicit zeros introduced stay within
// nrelax · (merged columns). The merged group's row pattern is the
// deepest member's pattern R_g = li[lp[g0] .. lp[g0+1]) (parent patterns
// are subsets of it in a Cholesky factor), so the group panel is
// |R_g| × C, and column o (0-based within the group) carries rows
// R_g[o..] (|R_g|-o structural entries). Hence:
//   merged_struct = Σ_{o=0}^{C-1}(|R_g|-o) = C·|R_g| - C(C-1)/2
//   extra_zeros   = merged_struct - Σ_{j∈group} colcount[j]
// Amalgamation errors are PERF-only (the row pattern is the exact union,
// so any merge yields a correct factor); nrelax trades panel size vs
// explicit zeros and is tuned by the close benchmark.
// =======================================================================

SupernodalSymbolic build_supernodal_symbolic(const ordering::SymbolicFactor& sf, crd::memory::IAllocator* alloc,
                                             crd::u32 nrelax)
{
    SupernodalSymbolic out(alloc);
    out.n = sf.n;
    const crd::u32 nf = sf.nsuper;
    if (sf.n == 0 || nf == 0)
    {
        out.scol.resize(1);
        out.scol[0] = 0;
        out.srowp.resize(1);
        out.srowp[0] = 0;
        out.col_super.resize(sf.n);
        return out;
    }

    // Column → fundamental supernode.
    crd::containers::Array<crd::u32> col_fs(alloc);
    col_fs.resize(sf.n);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 c = sf.super[f]; c < sf.super[f + 1]; ++c)
        {
            col_fs[c] = f;
        }
    }

    // Etree-parent supernode of fundamental f (kNoParent if a root).
    auto parent_fs = [&](crd::u32 f) -> crd::u32
    {
        const crd::u32 last = sf.super[f + 1] - 1;
        const crd::u32 p = sf.parent[last];
        return (p == ordering::kNoParent) ? ordering::kNoParent : col_fs[p];
    };
    // colcount[c] == li entries of column c == lp[c+1]-lp[c].
    auto colcount = [&](crd::u32 c) -> crd::u64
    {
        return static_cast<crd::u64>(sf.lp[c + 1] - sf.lp[c]);
    };

    // Chain-merge along the etree in postorder. The merged supernode's row
    // pattern is the genuine UNION of its fundamental members' patterns — a
    // parent's below-rows are NOT a subset of the child's (e.g. tridiagonal:
    // col j = {j,j+1}, parent j+1 = {j+1,j+2}), so the union, not the deepest
    // child, is the panel's row set. The group's columns are the union's
    // smallest |C| entries (each column's pattern starts at its own diagonal),
    // so they form the dense diagonal block in row-pattern order.
    auto union_into = [](const crd::containers::Array<crd::u32>& a, const crd::u32* b, crd::u32 bn,
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
    };

    // Leading-column L pattern of fundamental supernode `fi`: from the compact `slead` (supernodal
    // symbolic, no full li) when present, else from the full `li`. Same ascending diagonal-first
    // entries either way ⇒ identical supernode amalgamation + numeric factor.
    const bool use_slead = !sf.slead_ptr.empty();
    auto lead_pat = [&](crd::u32 fi, const crd::u32*& ptr, crd::u32& cnt)
    {
        if (use_slead)
        {
            const crd::u32 b = sf.slead_ptr[fi];
            ptr = &sf.slead_idx[b];
            cnt = sf.slead_ptr[fi + 1] - b;
        }
        else
        {
            const crd::u32 fc = sf.super[fi];
            ptr = &sf.li[sf.lp[fc]];
            cnt = sf.lp[fc + 1] - sf.lp[fc];
        }
    };

    out.scol.push_back(0);
    out.srowp.push_back(0);
    crd::containers::Array<crd::u32> merged(alloc);
    crd::containers::Array<crd::u32> cand(alloc);
    crd::u32 ns = 0;
    crd::u32 f = 0;
    while (f < nf)
    {
        const crd::u32 g = f;
        merged.clear();
        const crd::u32* gp = nullptr;
        crd::u32 gn = 0;
        lead_pat(g, gp, gn); // fundamental g's leading pattern (ascending, diagonal incl.)
        for (crd::u32 q = 0; q < gn; ++q)
        {
            merged.push_back(gp[q]);
        }
        crd::u64 c_cols = sf.super[g + 1] - sf.super[g];
        crd::u64 fund_struct = 0;
        for (crd::u32 c = sf.super[g]; c < sf.super[g + 1]; ++c)
        {
            fund_struct += colcount(c);
        }
        crd::u32 cur = g;
        while (cur + 1 < nf && parent_fs(cur) == cur + 1)
        {
            const crd::u32 nxt = cur + 1;
            const crd::u32* np = nullptr;
            crd::u32 nn = 0;
            lead_pat(nxt, np, nn);
            union_into(merged, np, nn, cand);
            const crd::u64 cp = c_cols + (sf.super[nxt + 1] - sf.super[nxt]);
            const crd::u64 pp = cand.size();
            crd::u64 fund_nxt = 0;
            for (crd::u32 c = sf.super[nxt]; c < sf.super[nxt + 1]; ++c)
            {
                fund_nxt += colcount(c);
            }
            const crd::u64 trap = cp * pp - cp * (cp - 1) / 2; // merged panel's lower trapezoid
            const crd::u64 extra = trap - (fund_struct + fund_nxt);
            if (extra > static_cast<crd::u64>(nrelax) * cp)
            {
                break;
            }
            merged.clear(); // accept: merged := cand
            for (crd::u32 k = 0; k < cand.size(); ++k)
            {
                merged.push_back(cand[k]);
            }
            c_cols = cp;
            fund_struct += fund_nxt;
            cur = nxt;
        }
        for (crd::u32 k = 0; k < merged.size(); ++k)
        {
            out.srow.push_back(merged[k]);
        }
        out.srowp.push_back(static_cast<crd::u32>(out.srow.size()));
        out.scol.push_back(sf.super[cur + 1]); // one past the group's last column
        ++ns;
        f = cur + 1;
    }
    out.nsuper = ns;
    out.col_super.resize(sf.n);
    for (crd::u32 s = 0; s < ns; ++s)
    {
        for (crd::u32 c = out.scol[s]; c < out.scol[s + 1]; ++c)
        {
            out.col_super[c] = s;
        }
    }
    // Factor fill = the lower TRAPEZOID per panel (column o carries |R_s|-o rows)
    // = |R_s|·cols - cols(cols-1)/2, comparable to sf.nnz() + the frontier fill
    // metric. (The dense-rectangle storage |R_s|·cols is a separate quantity the
    // numeric step allocates.) With amalgamation this includes the explicit zeros,
    // so it is always ≥ the un-amalgamated nnz(L).
    out.lnz = 0;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        const crd::u64 rg = out.srowp[s + 1] - out.srowp[s];
        const crd::u64 cols = out.scol[s + 1] - out.scol[s];
        out.lnz += rg * cols - cols * (cols - 1) / 2;
    }
    return out;
}

// =======================================================================
// v5a-1b steps 2-5 — numeric factorization + solve (left-looking
// supernodal). cmod uses dense `gemm` (build on our kernels); cdiv uses
// `factor_cholesky` for the diagonal block + a hand-rolled subdiagonal
// forward-solve (the lu.cpp inner-panel precedent). Pending updates routed
// via the CHOLMOD-style Lpos/Head/Next lists. Serial ⇒ deterministic; the
// cross-thread moat is proven under tree-parallelism in v5a-3.
// =======================================================================

template <typename T>
SupernodalCholesky<T>::SupernodalCholesky(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_sym(alloc), m_lx(alloc), m_lxp(alloc), m_lvl_ptr(alloc), m_lvl_list(alloc), m_upd_ptr(alloc),
      m_upd_list(alloc)
{
}

template <typename T>
void SupernodalCholesky<T>::factorize(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                                      crd::u32 nrelax, crd::u32 num_workers)
{
    m_info = 0;
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    g_scaleprof.reset();
    const auto fac_t0 = ScaleClock::now();
#endif
    const ordering::SymbolicFactor sf = ordering::symbolic_factorize(pattern, m_alloc, /*supernodal_patterns=*/true);
    m_sym = build_supernodal_symbolic(sf, m_alloc, nrelax);
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    g_scaleprof.sym_wall_ns = scale_ns(fac_t0, ScaleClock::now());
#endif
    const SupernodalSymbolic& sym = m_sym;
    m_n = sym.n;
    m_lnz = sym.lnz;
    const crd::u32 ns = sym.nsuper;
    crd::u32 cdiv_block = kCdivBlock;
#ifdef CRD_HESAP_CHOL_PROFILE
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv — temp profiling only
#endif
    if (const char* cb = std::getenv("CRD_CDIV_BLOCK"))
    {
        cdiv_block = static_cast<crd::u32>(std::strtoul(cb, nullptr, 10));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif

    m_lxp.resize(ns + 1);
    m_lxp[0] = 0;
    crd::u32 max_nr = 0;
    crd::u32 max_nc = 0;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        const crd::u32 nr = sym.srowp[s + 1] - sym.srowp[s];
        const crd::u32 nc = sym.scol[s + 1] - sym.scol[s];
        m_lxp[s + 1] = m_lxp[s] + static_cast<crd::u32>(static_cast<crd::u64>(nr) * nc);
        max_nr = nr > max_nr ? nr : max_nr;
        max_nc = nc > max_nc ? nc : max_nc;
    }
    m_lx.resize(m_lxp[ns]);
    for (crd::usize i = 0; i < m_lx.size(); ++i)
    {
        m_lx[i] = T{0};
    }
    if (ns == 0)
    {
        return;
    }

    // Static per-supernode update lists (parallel-safe — replaces the serial Head/Next
    // dynamic lists). upd_list[upd_ptr[s] .. upd_ptr[s+1]) = the descendant supernodes
    // K that contribute to s (ascending K), i.e. K has a below-diagonal row in s's
    // columns. Built in one pass over each K's below-pattern (rows ascending ⇒
    // col_super non-decreasing ⇒ distinct ancestors dedup by run). Read-only during
    // factorization ⇒ each supernode's cmod is independent (the v5a-3 parallelism).
    crd::containers::Array<crd::u32> upd_ptr(m_alloc);
    crd::containers::Array<crd::u32> upd_list(m_alloc);
    upd_ptr.resize(ns + 1);
    for (crd::u32 s = 0; s <= ns; ++s)
    {
        upd_ptr[s] = 0;
    }
    for (crd::u32 k = 0; k < ns; ++k)
    {
        const crd::u32 krb = sym.srowp[k];
        const crd::u32 knr = sym.srowp[k + 1] - krb;
        const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
        crd::u32 prev = ns; // sentinel
        for (crd::u32 i = knc; i < knr; ++i)
        {
            const crd::u32 anc = sym.col_super[sym.srow[krb + i]];
            if (anc != prev)
            {
                ++upd_ptr[anc + 1];
                prev = anc;
            }
        }
    }
    for (crd::u32 s = 0; s < ns; ++s)
    {
        upd_ptr[s + 1] += upd_ptr[s];
    }
    upd_list.resize(upd_ptr[ns]);
    {
        crd::containers::Array<crd::u32> ufill(m_alloc);
        ufill.resize(ns);
        for (crd::u32 s = 0; s < ns; ++s)
        {
            ufill[s] = upd_ptr[s];
        }
        for (crd::u32 k = 0; k < ns; ++k)
        {
            const crd::u32 krb = sym.srowp[k];
            const crd::u32 knr = sym.srowp[k + 1] - krb;
            const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
            crd::u32 prev = ns;
            for (crd::u32 i = knc; i < knr; ++i)
            {
                const crd::u32 anc = sym.col_super[sym.srow[krb + i]];
                if (anc != prev)
                {
                    upd_list[ufill[anc]++] = k;
                    prev = anc;
                }
            }
        }
    }
    // Per-worker scratch (v5a-3): relrow (global row → local panel row) + the cmod
    // Schur-update buffer, one slice per worker so concurrent supernodes never share.
    // Size by jobs::num_workers() (the pool worker_index() ranges over), index by
    // worker_index() — per feedback_jobs_worker_index_aliasing. Serial ⇒ 1 slice.
    const crd::u32 sw = (num_workers <= 1) ? 1U : crd::jobs::num_workers();
    const crd::usize ustride = static_cast<crd::usize>(max_nr) * max_nc;
    crd::containers::Array<crd::u32> relrow(m_alloc);
    relrow.resize(static_cast<crd::usize>(sw) * m_n);
    for (crd::usize i = 0; i < relrow.size(); ++i)
    {
        relrow[i] = m_n; // sentinel
    }
    // Per-worker lrmap: descendant-row → target-local-row, precomputed once per cmod
    // descendant so the scatter-subtract inner loop reads lrmap[pr] (1 load) instead of
    // rr[srow[..]] (2 loads) recomputed m1× per row.
    crd::containers::Array<crd::u32> relmap(m_alloc);
    relmap.resize(static_cast<crd::usize>(sw) * max_nr);
    crd::containers::Array<T> ubuf(m_alloc);
    ubuf.resize(static_cast<crd::usize>(sw) * ustride);
    // Per-worker bw×bw triangular-inverse scratch (cdiv below-block BLAS-3 path):
    // L11⁻¹ of the diagonal block, so the below-block solve L21 = A21·L11⁻ᴴ is a gemm.
    crd::containers::Array<T> linvbuf(m_alloc);
    const crd::usize linv_stride = static_cast<crd::usize>(cdiv_block) * cdiv_block;
    linvbuf.resize(static_cast<crd::usize>(sw) * linv_stride);
    std::atomic<crd::u32> fail_col{0xFFFFFFFFU};

    // Factor one supernode into its OWN panel: scatter A + cmod (reads descendants'
    // already-factored panels, fixed ascending-K order ⇒ deterministic) + in-place
    // cdiv. Writes only panel s + the worker's scratch ⇒ race-free across supernodes.
    // `par_workers` > 1 ⇒ this supernode is factored NODE-PARALLEL (it sits in a thin
    // etree level): its big panel gemms use `gemm_parallel` across the idle worker pool.
    // par_workers ≤ 1 ⇒ serial gemm (the tree-parallel path; one worker per supernode).
    // gemm_parallel ≡ serial gemm bit-for-bit ⇒ the factor is identical either way.
    auto chol_gemm = [&](T alpha, dense::MatrixView<const T, dense::Layout::ColMajor> a,
                         dense::MatrixView<const T, dense::Layout::ColMajor> b, T beta,
                         dense::MatrixView<T, dense::Layout::ColMajor> c, dense::Trans ta, dense::Trans tb,
                         crd::u32 par_workers)
    {
        const crd::u64 flop = static_cast<crd::u64>(2) * c.rows() * c.cols() * a.cols();
        if (par_workers > 1 && flop >= kGemmParallelMinFlop)
        {
            dense::gemm_parallel<T, dense::Layout::ColMajor>(par_workers, alpha, a, b, beta, c, ta, tb, nullptr);
            crd::jobs::frame_reset(); // reclaim this gemm's JobDecls (node-parallel runs many serially on main)
        }
        else
        {
            dense::gemm<T, dense::Layout::ColMajor>(alpha, a, b, beta, c, ta, tb, nullptr);
        }
    };
    auto factor_one = [&](crd::u32 s, crd::u32 worker, crd::u32 par_workers)
    {
        crd::u32* rr = relrow.data() + static_cast<crd::usize>(worker) * m_n;
        T* ub = ubuf.data() + static_cast<crd::usize>(worker) * ustride;
        T* linv = linvbuf.data() + static_cast<crd::usize>(worker) * linv_stride; // cdiv L11⁻¹ scratch
        // (cmod's scatter row-map now lives in cmod_slab as per-LANE lrm_w; the old front-worker
        // lrm was unused once cmod went row-slab-parallel.)
        const crd::u32 rb = sym.srowp[s];
        const crd::u32 nr = sym.srowp[s + 1] - rb;
        const crd::u32 firstcol = sym.scol[s];
        const crd::u32 nc = sym.scol[s + 1] - firstcol;
        T* panel = &m_lx[m_lxp[s]];
        for (crd::u32 a = 0; a < nr; ++a)
        {
            rr[sym.srow[rb + a]] = a;
        }
        // Panels are COLUMN-MAJOR (ld = nr): entry (row, col) at panel[col*nr + row].
        // ColMajor gives contiguous COLUMNS, which the right-looking cdiv writes and the
        // solve's subdiagonal axpy read as long SIMD runs (the solve-crush layout).
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t0 = ProfClock::now();
#endif
        for (crd::u32 c = firstcol; c < firstcol + nc; ++c) // scatter A's lower triangle
        {
            const crd::u32 lc = c - firstcol;
            const crd::u32 st = pattern.outer_ptr[c];
            const crd::u32 cnt = pattern.inner_count(c);
            for (crd::u32 kk = 0; kk < cnt; ++kk)
            {
                const crd::u32 i = pattern.inner_idx[st + kk];
                if (i >= c)
                {
                    // outer = rows (CSR): this reads A[c][i] (i>=c, upper triangle by row).
                    // L's column c needs the LOWER-triangle source A[i][c] = conj(A[c][i])
                    // for Hermitian (identity for real-symmetric).
                    panel[static_cast<crd::usize>(lc) * nr + rr[i]] = chol_conj<T>(values[st + kk]);
                }
            }
        }
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t1 = ProfClock::now();
        g_cholprof.scatter_ns += prof_ns(prof_t0, prof_t1);
#endif
        // cmod — Schur updates from descendants. NODE-PARALLEL fronts (par_workers>1) partition the
        // front's ROWS across the worker pool with ONE fork per front — NOT gemm_parallel per
        // descendant, which paid fork/join per call and capped the huge-front cmod scaling at ~1.86×
        // (measured). The cmod gemm SHAPE parallelizes 4.5–6.8× under OpenBLAS (ob_probe), so the cap
        // was Cerid's per-descendant fork/join, not the shape. Each lane owns a front-local row-slab
        // [r0,r1); for every descendant it does the sub-gemm over the descendant rows mapping into the
        // slab — a CONTIGUOUS pr-range because lrm[pr]=rr[srow[..]] is strictly increasing (rr is the
        // position-in-sorted-front-rows; the descendant's rows are a sorted subset) — and scatters
        // ONLY into its own rows. BIT-IDENTICAL to serial: each front row is owned by exactly one lane
        // and its subtracts are applied in ascending-descendant order = the serial order; each gemm
        // output element's K-reduction is independent of the row-slab (the property gemm_parallel
        // already relies on). rr is filled once (this front's worker slot, 615–618) and read-only
        // across lanes; ub/lrm are per-LANE scratch (lane worker_index, never the front's worker).
        auto cmod_slab = [&](crd::u32 r0, crd::u32 r1, crd::u32 w)
        {
            T* ub_w = ubuf.data() + static_cast<crd::usize>(w) * ustride;
            crd::u32* lrm_w = relmap.data() + static_cast<crd::usize>(w) * max_nr;
            const crd::u32 g_lo = sym.srow[rb + r0]; // slab global-row lower bound (inclusive)
            const crd::u32 g_hi = (r1 < nr) ? sym.srow[rb + r1] : (sym.srow[rb + nr - 1] + 1); // exclusive
            for (crd::u32 ui = upd_ptr[s]; ui < upd_ptr[s + 1]; ++ui)
            {
                const crd::u32 k = upd_list[ui];
                const crd::u32 krb = sym.srowp[k];
                const crd::u32 knr = sym.srowp[k + 1] - krb;
                const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
                const T* kpanel = &m_lx[m_lxp[k]];
                crd::u32 lo = knc;
                crd::u32 hi = knr;
                while (lo < hi) // p0 = first K-row ≥ firstcol
                {
                    const crd::u32 mid = lo + (hi - lo) / 2;
                    if (sym.srow[krb + mid] < firstcol)
                    {
                        lo = mid + 1;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                const crd::u32 p0 = lo;
                crd::u32 m1 = 0;
                while (p0 + m1 < knr && sym.srow[krb + p0 + m1] < firstcol + nc)
                {
                    ++m1;
                }
                const crd::u32 msz = knr - p0;
                // Contiguous pr sub-range of this descendant's U-rows (srow[krb+p0 .. krb+knr), sorted)
                // mapping into the slab's global-row window [g_lo, g_hi). Binary-search both ends.
                crd::u32 a = 0;
                crd::u32 b = msz;
                while (a < b)
                {
                    const crd::u32 m = a + (b - a) / 2;
                    if (sym.srow[krb + p0 + m] < g_lo)
                    {
                        a = m + 1;
                    }
                    else
                    {
                        b = m;
                    }
                }
                const crd::u32 pr_lo = a;
                b = msz;
                a = pr_lo;
                while (a < b)
                {
                    const crd::u32 m = a + (b - a) / 2;
                    if (sym.srow[krb + p0 + m] < g_hi)
                    {
                        a = m + 1;
                    }
                    else
                    {
                        b = m;
                    }
                }
                const crd::u32 pr_hi = a;
                if (pr_lo >= pr_hi)
                {
                    continue; // descendant touches no row in this slab
                }
                const crd::u32 sub = pr_hi - pr_lo;
                // Sub-gemm over rows [pr_lo,pr_hi): SERIAL (the front-level fork is already paid).
                // f64 REROUTE: Uᵀ(m1×sub, ld=sub) = am1·am[slab]ᵀ on the fast RowMajor-C path. Uᵀ
                // RowMajor == U ColMajor (sub×m1) in the SAME memory ⇒ the ColMajor scatter is unchanged.
                if constexpr (std::is_same_v<T, crd::f64>)
                {
                    const dense::MatrixView<const T, dense::Layout::RowMajor> am1r(kpanel + static_cast<crd::usize>(p0),
                                                                                   knc, m1, knr); // = am1ᵀ (full m1)
                    const dense::MatrixView<const T, dense::Layout::RowMajor> amr(
                        kpanel + static_cast<crd::usize>(p0 + pr_lo), knc, sub, knr);            // = am[slab]ᵀ
                    const dense::MatrixView<T, dense::Layout::RowMajor> utr(ub_w, m1, sub, sub); // Uᵀ sub RowMajor
                    dense::gemm<T, dense::Layout::RowMajor>(T{1}, am1r, amr, T{0}, utr, dense::Trans::Transpose,
                                                            dense::Trans::None, nullptr);
                }
                else
                {
                    const dense::MatrixView<const T, dense::Layout::ColMajor> am(
                        kpanel + static_cast<crd::usize>(p0 + pr_lo), sub, knc, knr);
                    const dense::MatrixView<const T, dense::Layout::ColMajor> am1(kpanel + static_cast<crd::usize>(p0),
                                                                                  m1, knc, knr);
                    const dense::MatrixView<T, dense::Layout::ColMajor> uvw(ub_w, sub, m1, sub); // U sub ColMajor ld=sub
                    dense::gemm<T, dense::Layout::ColMajor>(T{1}, am, am1, T{0}, uvw, dense::Trans::None,
                                                            kCholAdjoint<T>, nullptr);
                }
                for (crd::u32 pr = pr_lo; pr < pr_hi; ++pr) // descendant-row → target-local-row
                {
                    lrm_w[pr] = rr[sym.srow[krb + p0 + pr]];
                }
                for (crd::u32 pc = 0; pc < m1; ++pc)
                {
                    const crd::u32 gcol = sym.srow[krb + p0 + pc];
                    T* pcoldst = panel + static_cast<crd::usize>(gcol - firstcol) * nr; // target column
                    const T* ubc = ub_w + static_cast<crd::usize>(pc) * sub; // U[pr,pc] = ubc[pr-pr_lo] (both layouts)
                    for (crd::u32 pr = pr_lo; pr < pr_hi; ++pr)
                    {
                        pcoldst[lrm_w[pr]] -= ubc[pr - pr_lo]; // only this lane's rows ⇒ disjoint
                    }
                }
            }
        };
        if (par_workers <= 1)
        {
            cmod_slab(0, nr, worker); // serial / tree-parallel: one slab = the whole front
        }
        else
        {
            // Node-parallel huge front: ONE fork over the front's row-slabs (amortizes the per-front
            // fork/join the per-descendant gemm_parallel paid). rr (615–618) is filled and read-only.
            // (Over-decomposing num_jobs=par_workers*K was measured NULL on the real all-matrix workload —
            // the residual is load imbalance the cheap knob didn't capture; guided/work-stealing chunking
            // is a characterized future lever, not kept here.)
            auto* counter = crd::jobs::parallel_for(nr, par_workers,
                                                    [&](crd::u32 bb, crd::u32 ee)
                                                    { cmod_slab(bb, ee, crd::jobs::worker_index()); });
            crd::jobs::wait(counter);
            crd::jobs::frame_reset(); // reclaim this front's parallel_for JobDecls
        }
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t2 = ProfClock::now();
        g_cholprof.cmod_ns += prof_ns(prof_t1, prof_t2);
        const int prof_cdiv_bin = CholProf::cdiv_bin(nc);
        crd::u64 prof_cdiv_flop = 0;
        for (crd::u32 jp = 0; jp < nc; ++jp) // lower-trapezoid Cholesky multiply-adds
        {
            prof_cdiv_flop += (nr - jp - 1); // scale
            for (crd::u32 jjp = jp + 1; jjp < nc; ++jjp)
            {
                prof_cdiv_flop += static_cast<crd::u64>(2) * (nr - jjp); // rank-1
            }
        }
        g_cholprof.cdiv_flops += prof_cdiv_flop;
        g_cholprof.cdiv_flop_bin[prof_cdiv_bin] += prof_cdiv_flop;
#endif
        // cdiv — factor the supernode panel. The old whole-panel unblocked rank-1 sweep
        // was O(nc²·nr) BLAS-1 (AI≈0.33) — the CHOLMOD per-thread gap on fat supernodes
        // (bmwcra maxnc=2406). Two paths: THIN supernodes (nc ≤ kCdivScalarMin) keep the
        // pure-scalar rank-1 (negligible flops, but numerous ⇒ no per-supernode BLAS-3
        // overhead); FAT supernodes go BLOCKED — scalar bw×bw diagonal + BLAS-3 below-block
        // (L21 = A21·L11⁻ᴴ via invert(L11)+gemm, the AI win) + BLAS-3 trailing Schur gemm.
        bool cdiv_failed = false;
        if (nc <= kCdivScalarMin)
        {
            for (crd::u32 j = 0; j < nc; ++j) // THIN: full-panel scalar rank-1 (pre-v5a-4 path)
            {
                const crd::usize cj = static_cast<crd::usize>(j) * nr;
                const dense::RealType<T> djj = chol_real<T>(panel[cj + j]);
                if (!(djj > dense::RealType<T>{0}))
                {
                    const crd::u32 fc = firstcol + j + 1;
                    crd::u32 cur = fail_col.load(std::memory_order_relaxed);
                    while (fc < cur && !fail_col.compare_exchange_weak(cur, fc, std::memory_order_relaxed))
                    {
                    }
                    cdiv_failed = true;
                    break;
                }
                const dense::RealType<T> d = std::sqrt(djj);
                panel[cj + j] = chol_from_real<T>(d);
                const dense::RealType<T> invd = dense::RealType<T>{1} / d;
                for (crd::u32 i = j + 1; i < nr; ++i)
                {
                    panel[cj + i] *= invd;
                }
                for (crd::u32 jj = j + 1; jj < nc; ++jj)
                {
                    const crd::usize cjj = static_cast<crd::usize>(jj) * nr;
                    const T f = chol_conj<T>(panel[cj + jj]);
                    for (crd::u32 i = jj; i < nr; ++i)
                    {
                        panel[cjj + i] -= panel[cj + i] * f;
                    }
                }
            }
        }
        else
        {
            // TWO-LEVEL blocked panel Cholesky (LAPACK xPOTRF structure; v5a-4 cdiv crush).
            // OUTER block obw (= cdiv_block) is the trailing Schur gemm's K dimension (step B):
            // wide ⇒ high arithmetic intensity + few large gemm calls. The OUTER panel
            // [ko:koend] (diagonal + below over ALL rows) is itself factored by an INNER
            // kCdivInnerBlock-blocked right-looking sweep (step A) whose ONLY scalar work is the
            // ibw×ibw POTF2 + ibw×ibw invert — total O(nc·ibw²), INDEPENDENT of obw. This is what
            // the naive single-level sweep lacked: its O(obw²·nc) scalar diagonal capped it at
            // bw=128 (bw=256 measured WORSE). Every gemm is bit-identical serial/parallel
            // (gemm_parallel ≡ serial, disjoint row-slabs) ⇒ the cross-thread determinism moat holds.
            const crd::u32 inner_bw = kCdivInnerBlock;
            for (crd::u32 ko = 0; ko < nc && !cdiv_failed; ko += cdiv_block)
            {
                const crd::u32 obw = (ko + cdiv_block < nc) ? cdiv_block : (nc - ko);
                const crd::u32 koend = ko + obw;
                // (A) factor the outer panel P[ko:nr, ko:koend], INNER-blocked right-looking.
                for (crd::u32 ki = ko; ki < koend && !cdiv_failed; ki += inner_bw)
                {
                    const crd::u32 ibw = (ki + inner_bw < koend) ? inner_bw : (koend - ki);
                    const crd::u32 iend = ki + ibw;
                    // (A1) scalar POTF2 of the ibw×ibw diagonal block [ki:iend, ki:iend].
                    for (crd::u32 j = ki; j < iend; ++j)
                    {
                        const crd::usize cj = static_cast<crd::usize>(j) * nr;
                        const dense::RealType<T> djj = chol_real<T>(panel[cj + j]);
                        if (!(djj > dense::RealType<T>{0}))
                        {
                            const crd::u32 fc = firstcol + j + 1;
                            crd::u32 cur = fail_col.load(std::memory_order_relaxed);
                            while (fc < cur && !fail_col.compare_exchange_weak(cur, fc, std::memory_order_relaxed))
                            {
                            }
                            cdiv_failed = true;
                            break;
                        }
                        const dense::RealType<T> d = std::sqrt(djj);
                        panel[cj + j] = chol_from_real<T>(d);
                        const dense::RealType<T> invd = dense::RealType<T>{1} / d;
                        for (crd::u32 i = j + 1; i < iend; ++i) // scale WITHIN the inner diagonal block
                        {
                            panel[cj + i] *= invd;
                        }
                        for (crd::u32 jj = j + 1; jj < iend; ++jj) // rank-1 WITHIN the inner diagonal block
                        {
                            const crd::usize cjj = static_cast<crd::usize>(jj) * nr;
                            const T f = chol_conj<T>(panel[cj + jj]);
                            for (crd::u32 i = jj; i < iend; ++i)
                            {
                                panel[cjj + i] -= panel[cj + i] * f;
                            }
                        }
                    }
                    if (cdiv_failed)
                    {
                        break;
                    }
                    // (A2) below-solve WITHIN the obw block: [iend:koend]×[ki:iend] = A·L11⁻ᴴ (K=ibw).
                    // (Below-OUTER rows [koend:nr] are deferred to the blocked trsm (B) — at K=obw.)
                    const crd::u32 below = koend - iend;
                    if (below > 0)
                    {
                        invert_lower_tri<T>(panel + static_cast<crd::usize>(ki) * nr + ki, nr, ibw, linv);
                        T* a21 = panel + static_cast<crd::usize>(ki) * nr + iend; // L[iend:koend, ki:iend], ld=nr
                        for (crd::u32 c = 0; c < ibw; ++c) // copy A21 (below×ibw) → ub; avoids gemm alias
                        {
                            const T* src = a21 + static_cast<crd::usize>(c) * nr;
                            T* dst = ub + static_cast<crd::usize>(c) * below;
                            for (crd::u32 r = 0; r < below; ++r)
                            {
                                dst[r] = src[r];
                            }
                        }
                        const dense::MatrixView<const T, dense::Layout::ColMajor> a21v(ub, below, ibw, below);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> linvv(linv, ibw, ibw, ibw);
                        const dense::MatrixView<T, dense::Layout::ColMajor> l21v(a21, below, ibw, nr);
                        // FORCE SERIAL (par_workers→1): A2 is bounded small (below≤obw−ibw≤192, K=ibw=64), so
                        // node-parallel gemm_parallel here only paid fork/join — a chief 8T cdiv-chain inflator.
                        chol_gemm(T{1}, a21v, linvv, T{0}, l21v, dense::Trans::None, kCholAdjoint<T>, 1U);
                    }
                    // (A3) trailing update WITHIN the obw block: cols [iend:koend], rows [iend:koend] (K=ibw).
                    if (iend < koend)
                    {
                        const crd::u32 tr = koend - iend;                                  // within-obw rows AND cols
                        const T* a_base = panel + static_cast<crd::usize>(ki) * nr + iend; // L[iend:koend, ki:iend]
                        T* c_base = panel + static_cast<crd::usize>(iend) * nr + iend;     // P[iend:koend, iend:koend]
                        const dense::MatrixView<const T, dense::Layout::ColMajor> av(a_base, tr, ibw, nr);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> bv(a_base, tr, ibw, nr);
                        const dense::MatrixView<T, dense::Layout::ColMajor> cv(c_base, tr, tr, nr);
                        // FORCE SERIAL (par_workers→1): A3 within-obw trailing is bounded small (tr≤obw−ibw≤192,
                        // K=ibw=64) ⇒ gemm_parallel only paid fork/join (8T cdiv-chain inflator).
                        chol_gemm(T{-1}, av, bv, T{1}, cv, dense::Trans::None, kCholAdjoint<T>, 1U);
                    }
                }
                if (cdiv_failed)
                {
                    break;
                }
                // (B) below-OUTER block [koend:nr]×[ko:koend] = A21·L11⁻ᴴ via a BLOCKED trsm: the
                // gemm-UPDATE K grows (kacc: 0→64→128→…→obw-64) instead of the old per-inner-block
                // flat K=64 over all rows — the dominant below-block flops now run at high arithmetic
                // intensity (v5a-4 cdiv-A lever: OpenBLAS dtrsm = 65 GF/s on this shape, Cerid was 26).
                // L11 = the just-factored obw diagonal block. Each gemm is bit-identical serial/parallel.
                const crd::u32 below_o = nr - koend;
                // (B) ROW-SLAB PARALLEL (v5a-7 sub-slice 3): ONE fork per outer block over the below_o
                // rows — replaces the per-jb-block `gemm_parallel` forks + the wrong-axis (jbw≤64-col)
                // serial apply, which left the dominant below-outer work NOT scaling (the cdiv-chain wall;
                // the genuinely-serial POTF2 floor is ~0). Each lane owns a below_o row-slab [r0,r1) and
                // walks the jb-blocks SEQUENTIALLY: B1 (K=kacc-growing update) then B2 (jbw×jbw diagonal
                // solve), ascending jb — that IS the serial accumulation order ⇒ BIT-IDENTICAL; rows are
                // disjoint ⇒ race-free. Per-lane ub_w/linv_w (slab-relative offsets); L11 (bv + the
                // jbw×jbw diagonal) is SHARED read-only — the diagonal invert is redundant per lane
                // (64×64, noise vs the below_o×jbw gemms; sidesteps all sharing/ordering hazards).
                auto b_slab = [&](crd::u32 r0, crd::u32 r1, crd::u32 w)
                {
                    T* ub_w = ubuf.data() + static_cast<crd::usize>(w) * ustride;
                    T* linv_w = linvbuf.data() + static_cast<crd::usize>(w) * linv_stride;
                    const crd::u32 slab = r1 - r0;
                    for (crd::u32 jb = ko; jb < koend; jb += inner_bw)
                    {
                        const crd::u32 jbw = (jb + inner_bw < koend) ? inner_bw : (koend - jb);
                        const crd::u32 kacc = jb - ko;                                       // already-solved cols [ko:jb)
                        T* xjb = panel + static_cast<crd::usize>(jb) * nr + koend + r0;       // X[r0:r1, jb-block]
                        if (kacc > 0) // (B1) X[r0:r1, jb] -= X[r0:r1, ko:jb] · L11[jb, ko:jb]ᴴ  (K=kacc, GROWS)
                        {
                            const T* a_base = panel + static_cast<crd::usize>(ko) * nr + koend + r0; // X[r0:r1, ko:jb]
                            const T* b_base = panel + static_cast<crd::usize>(ko) * nr + jb;         // L11[jb, ko:jb] (shared)
                            if constexpr (std::is_same_v<T, crd::f64>)
                            {
                                // f64 REROUTE: Tᵀ(jbw×slab, ld=slab) = bv·avᵀ on the fast RowMajor path; RowMajor Tᵀ
                                // IS T ColMajor(slab×jbw, ld=slab) in ub_w ⇒ the ColMajor subtract is unchanged.
                                const dense::MatrixView<const T, dense::Layout::RowMajor> av_r(a_base, kacc, slab, nr);
                                const dense::MatrixView<const T, dense::Layout::RowMajor> bv_r(b_base, kacc, jbw, nr);
                                const dense::MatrixView<T, dense::Layout::RowMajor> tt_r(ub_w, jbw, slab, slab);
                                dense::gemm<T, dense::Layout::RowMajor>(T{1}, bv_r, av_r, T{0}, tt_r,
                                                                        dense::Trans::Transpose, dense::Trans::None,
                                                                        nullptr);
                                for (crd::u32 c = 0; c < jbw; ++c) // X[r0:r1, jb] -= T (ColMajor in ub_w, ld=slab)
                                {
                                    T* cc = xjb + static_cast<crd::usize>(c) * nr;
                                    const T* tt = ub_w + static_cast<crd::usize>(c) * slab;
                                    for (crd::u32 r = 0; r < slab; ++r)
                                    {
                                        cc[r] -= tt[r];
                                    }
                                }
                            }
                            else
                            {
                                const dense::MatrixView<const T, dense::Layout::ColMajor> av(a_base, slab, kacc, nr);
                                const dense::MatrixView<const T, dense::Layout::ColMajor> bv(b_base, jbw, kacc, nr);
                                const dense::MatrixView<T, dense::Layout::ColMajor> cv(xjb, slab, jbw, nr);
                                chol_gemm(T{-1}, av, bv, T{1}, cv, dense::Trans::None, kCholAdjoint<T>, 1U); // serial
                            }
                        }
                        // (B2) X[r0:r1, jb] = X · L11[jb,jb]⁻ᴴ. Redundant per-lane jbw×jbw invert (64×64, noise).
                        invert_lower_tri<T>(panel + static_cast<crd::usize>(jb) * nr + jb, nr, jbw, linv_w);
                        for (crd::u32 c = 0; c < jbw; ++c) // copy X[r0:r1, jb] (slab×jbw) → ub_w; avoids alias
                        {
                            const T* src = xjb + static_cast<crd::usize>(c) * nr;
                            T* dst = ub_w + static_cast<crd::usize>(c) * slab;
                            for (crd::u32 r = 0; r < slab; ++r)
                            {
                                dst[r] = src[r];
                            }
                        }
                        const dense::MatrixView<const T, dense::Layout::ColMajor> xv(ub_w, slab, jbw, slab);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> linvv(linv_w, jbw, jbw, jbw);
                        const dense::MatrixView<T, dense::Layout::ColMajor> xout(xjb, slab, jbw, nr);
                        chol_gemm(T{1}, xv, linvv, T{0}, xout, dense::Trans::None, kCholAdjoint<T>, 1U); // serial
                    }
                };
                if (below_o > 0)
                {
                    if (par_workers <= 1)
                    {
                        b_slab(0, below_o, worker); // serial / tree-parallel: one slab = all below-outer rows
                    }
                    else
                    {
                        // Node-parallel huge front: ONE fork over the below_o rows (not per-jb-block).
                        auto* counter = crd::jobs::parallel_for(below_o, par_workers,
                                                                [&](crd::u32 bb, crd::u32 ee)
                                                                { b_slab(bb, ee, crd::jobs::worker_index()); });
                        crd::jobs::wait(counter);
                        crd::jobs::frame_reset();
                    }
                }
                // (C) OUTER trailing Schur update: cols [koend:nc], rows [koend:nr] (K=obw, high AI).
                if (koend < nc)
                {
                    const crd::u32 trail_rows = nr - koend;
                    const crd::u32 trail_cols = nc - koend;
                    const T* a_base = panel + static_cast<crd::usize>(ko) * nr + koend; // L[koend:nr, ko:koend]
                    T* c_base = panel + static_cast<crd::usize>(koend) * nr + koend;    // P[koend:nr, koend:nc]
#ifdef CRD_HESAP_CHOL_PROFILE
                    const auto prof_ot0 = ProfClock::now();
#endif
                    if constexpr (std::is_same_v<T, crd::f64>)
                    {
                        // f64 REROUTE (v5a-4 cdiv-B): the in-place ColMajor trailing gemm runs at Cerid's
                        // weak ColMajor-NT rate (~38 GF/s; OpenBLAS does it at 84). Compute the update Tᵀ =
                        // bv·avᵀ as a RowMajor-C gemm into ub — RowMajor Tᵀ(trail_cols×trail_rows,ld=trail_rows)
                        // IS T ColMajor(trail_rows×trail_cols,ld=trail_rows) in the SAME memory — onto Cerid's
                        // FAST RowMajor path (~60), then subtract T from the panel (ColMajor, contiguous per
                        // column). av_r/bv_r reinterpret the ColMajor sub-blocks as RowMajor (zero-copy transpose).
                        // gemm bit-identical serial/parallel + element-independent subtract ⇒ determinism moat
                        // holds. (Subtract is serial here; PARALLEL apply is the 8T follow-up — a serial pass
                        // taxes bmwcra's node-parallel critical path. TODO before 8T ship.)
                        const dense::MatrixView<const T, dense::Layout::RowMajor> av_r(a_base, obw, trail_rows, nr);
                        const dense::MatrixView<const T, dense::Layout::RowMajor> bv_r(a_base, obw, trail_cols, nr);
                        const dense::MatrixView<T, dense::Layout::RowMajor> tt_r(ub, trail_cols, trail_rows,
                                                                                 trail_rows);
                        const crd::u64 flop = static_cast<crd::u64>(2) * trail_rows * trail_cols * obw;
                        if (par_workers > 1 && flop >= kGemmParallelMinFlop)
                        {
                            dense::gemm_parallel<T, dense::Layout::RowMajor>(par_workers, T{1}, bv_r, av_r, T{0}, tt_r,
                                                                             dense::Trans::Transpose,
                                                                             dense::Trans::None, nullptr);
                            crd::jobs::frame_reset();
                        }
                        else
                        {
                            dense::gemm<T, dense::Layout::RowMajor>(
                                T{1}, bv_r, av_r, T{0}, tt_r, dense::Trans::Transpose, dense::Trans::None, nullptr);
                        }
                        // C -= T (T ColMajor in ub, ld=trail_rows), columns disjoint ⇒ deterministic.
                        // PARALLEL over columns for NODE-parallel fronts (par_workers>1 ⇒ factor_one on the
                        // dispatcher thread, so this parallel_for is NOT nested) — a serial apply here
                        // serialized the node-parallel critical path and REGRESSED hood/ldoor 8T (0.86→0.75).
                        // Tree-parallel (par_workers≤1, factor_one on a worker) stays serial (no nesting).
                        auto sub_col = [&](crd::u32 j)
                        {
                            T* cc = c_base + static_cast<crd::usize>(j) * nr;
                            const T* tt = ub + static_cast<crd::usize>(j) * trail_rows;
                            for (crd::u32 i = 0; i < trail_rows; ++i)
                            {
                                cc[i] -= tt[i];
                            }
                        };
                        if (par_workers > 1)
                        {
                            auto* sc = crd::jobs::parallel_for(trail_cols, par_workers,
                                                               [&](crd::u32 b, crd::u32 e)
                                                               {
                                                                   for (crd::u32 j = b; j < e; ++j)
                                                                   {
                                                                       sub_col(j);
                                                                   }
                                                               });
                            crd::jobs::wait(sc);
                            crd::jobs::frame_reset();
                        }
                        else
                        {
                            for (crd::u32 j = 0; j < trail_cols; ++j)
                            {
                                sub_col(j);
                            }
                        }
                    }
                    else
                    {
                        const dense::MatrixView<const T, dense::Layout::ColMajor> av(a_base, trail_rows, obw, nr);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> bv(a_base, trail_cols, obw, nr);
                        const dense::MatrixView<T, dense::Layout::ColMajor> cv(c_base, trail_rows, trail_cols, nr);
                        chol_gemm(T{-1}, av, bv, T{1}, cv, dense::Trans::None, kCholAdjoint<T>, par_workers);
                    }
#ifdef CRD_HESAP_CHOL_PROFILE
                    g_cholprof.cdiv_outertrail_ns += prof_ns(prof_ot0, ProfClock::now());
#endif
                }
            }
        }
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t3 = ProfClock::now();
        g_cholprof.cdiv_ns += prof_ns(prof_t2, prof_t3);
        g_cholprof.cdiv_ns_bin[prof_cdiv_bin] += prof_ns(prof_t2, prof_t3);
#endif
        for (crd::u32 a = 0; a < nr; ++a)
        {
            rr[sym.srow[rb + a]] = m_n; // restore sentinel for the next supernode on this worker
        }
    };

    // Supernode-etree levels: level[s] = 1 + max level over its update-list descendants
    // (each K < s, so processed in order). Same-level supernodes are mutually
    // independent ⇒ factor concurrently; levels run sequentially (ascending).
    crd::containers::Array<crd::u32> level(m_alloc);
    level.resize(ns);
    crd::u32 nlevels = 0;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        crd::u32 lv = 0;
        for (crd::u32 ui = upd_ptr[s]; ui < upd_ptr[s + 1]; ++ui)
        {
            const crd::u32 lk = level[upd_list[ui]] + 1;
            lv = lk > lv ? lk : lv;
        }
        level[s] = lv;
        nlevels = (lv + 1) > nlevels ? (lv + 1) : nlevels;
    }
    crd::containers::Array<crd::u32> lvl_ptr(m_alloc);
    crd::containers::Array<crd::u32> lvl_list(m_alloc);
    lvl_ptr.resize(nlevels + 1);
    for (crd::u32 l = 0; l <= nlevels; ++l)
    {
        lvl_ptr[l] = 0;
    }
    for (crd::u32 s = 0; s < ns; ++s)
    {
        ++lvl_ptr[level[s] + 1];
    }
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        lvl_ptr[l + 1] += lvl_ptr[l];
    }
    lvl_list.resize(ns);
    {
        crd::containers::Array<crd::u32> lf(m_alloc);
        lf.resize(nlevels);
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            lf[l] = lvl_ptr[l];
        }
        for (crd::u32 s = 0; s < ns; ++s)
        {
            lvl_list[lf[level[s]]++] = s; // ascending s within a level (determinism)
        }
    }
    // Store the etree levels for the level-parallel solve (copy out of the local scratch).
    m_nlevels = nlevels;
    m_lvl_ptr.resize(static_cast<crd::usize>(nlevels) + 1);
    for (crd::u32 l = 0; l <= nlevels; ++l)
    {
        m_lvl_ptr[l] = lvl_ptr[l];
    }
    m_lvl_list.resize(ns);
    for (crd::u32 s = 0; s < ns; ++s)
    {
        m_lvl_list[s] = lvl_list[s];
    }
    // Store the update-lists for the level-parallel LEFT-LOOKING forward solve (copy out of scratch).
    m_upd_ptr.resize(static_cast<crd::usize>(ns) + 1);
    for (crd::u32 s = 0; s <= ns; ++s)
    {
        m_upd_ptr[s] = upd_ptr[s];
    }
    m_upd_list.resize(upd_ptr[ns]);
    for (crd::u32 ui = 0; ui < upd_ptr[ns]; ++ui)
    {
        m_upd_list[ui] = upd_list[ui];
    }
    // Per-level flag: does the level hold a HUGE front (nc ≥ kNodeParallelMinCols)? Only such
    // levels go node-parallel; thin levels of only-small supernodes stay tree-parallel.
    crd::containers::Array<crd::u8> level_has_huge(m_alloc);
    level_has_huge.resize(nlevels == 0 ? 1 : nlevels);
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        level_has_huge[l] = 0;
    }
    for (crd::u32 s = 0; s < ns; ++s)
    {
        if (sym.scol[s + 1] - sym.scol[s] >= kNodeParallelMinCols)
        {
            level_has_huge[level[s]] = 1;
        }
    }

#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    g_scaleprof.setup_wall_ns = scale_ns(fac_t0, ScaleClock::now()); // symbolic + alloc + level build
#endif
#ifdef CRD_HESAP_CHOL_PROFILE
    g_cholprof.reset();
#endif
    if (num_workers <= 1)
    {
        for (crd::u32 s = 0; s < ns; ++s)
        {
            factor_one(s, 0, 1);
        }
    }
    else
    {
        // 2D-HYBRID schedule (the within-supernode-parallelism lever): a level is TREE-parallel
        // (one worker per supernode, serial gemm) UNLESS it is thin (cnt < num_workers) AND
        // holds a huge front — only then NODE-parallel (supernodes sequential on the dispatcher,
        // each driving gemm_parallel across the otherwise-idle pool for its big panel gemms).
        // This targets the few near-root huge fronts that serialize under pure tree-parallelism
        // WITHOUT serializing thin levels of small supernodes. Same factor either way
        // (gemm_parallel ≡ serial gemm — disjoint row-slabs ⇒ the determinism moat holds).
        const crd::u32* list = lvl_list.data();
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            const crd::u32 off = lvl_ptr[l];
            const crd::u32 cnt = lvl_ptr[l + 1] - off;
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
            const auto lvl_t0 = ScaleClock::now();
#endif
            if (cnt >= num_workers || level_has_huge[l] == 0)
            {
                auto* counter = crd::jobs::parallel_for(cnt, num_workers,
                                                        [&, off, list](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 w = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                factor_one(list[off + t], w, 1);
                                                            }
                                                        });
                crd::jobs::wait(counter);
            }
            else
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    factor_one(list[off + t], 0, num_workers); // node-parallel big gemms
                }
            }
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
            {
                const double w = scale_ns(lvl_t0, ScaleClock::now());
                ++g_scaleprof.n_levels;
                if (cnt < num_workers && level_has_huge[l] == 1)
                {
                    g_scaleprof.node_wall_ns += w;
                    ++g_scaleprof.n_node_levels;
                }
                else if (cnt < num_workers)
                {
                    g_scaleprof.starved_wall_ns += w; // tree-parallel but workers idle (starvation)
                    ++g_scaleprof.n_starved_levels;
                }
                else
                {
                    g_scaleprof.tree_wall_ns += w; // healthy: cnt >= num_workers
                }
                if (w > g_scaleprof.max_level_wall_ns)
                {
                    g_scaleprof.max_level_wall_ns = w;
                    g_scaleprof.max_level_cnt = cnt;
                    crd::u32 mnc = 0;
                    for (crd::u32 t = 0; t < cnt; ++t)
                    {
                        const crd::u32 s = list[off + t];
                        const crd::u32 sc = sym.scol[s + 1] - sym.scol[s];
                        mnc = sc > mnc ? sc : mnc;
                    }
                    g_scaleprof.max_level_nc = mnc;
                }
            }
#endif
            // Reclaim this level's JobDecls from the per-thread frame arena; a deep etree
            // (ldoor ~1M) issues thousands of parallel_for batches and the 1 MB frame arena
            // exhausts without a reset at each level boundary. (chol_gemm also resets per
            // node-parallel gemm_parallel; this bounds the tree-parallel batches too.)
            crd::jobs::frame_reset();
        }
    }

    const crd::u32 fc = fail_col.load(std::memory_order_relaxed);
    m_info = (fc == 0xFFFFFFFFU) ? 0 : static_cast<crd::usize>(fc);
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    if (num_workers > 1)
    {
        const double symw = g_scaleprof.sym_wall_ns * 1e-6;
        const double setupw = g_scaleprof.setup_wall_ns * 1e-6;
        const double allocw = setupw - symw; // scratch alloc + level building
        const double tw = g_scaleprof.tree_wall_ns * 1e-6;
        const double nw = g_scaleprof.node_wall_ns * 1e-6;
        const double stw = g_scaleprof.starved_wall_ns * 1e-6;
        const double numw = tw + nw + stw;
        const double tot = setupw + numw;
        std::printf("  [CHOLSCALE n=%u W=%u] SETUP=%.1fms(sym=%.1f alloc+lvl=%.1f) NUMERIC=%.1fms(tree=%.1f "
                    "node=%.1f STARVED=%.1f/%ulev) | max-lev=%.1fms cnt=%u nc=%u | SETUP%%=%.0f node%%=%.0f\n",
                    m_n, num_workers, setupw, symw, allocw, numw, tw, nw, stw, g_scaleprof.n_starved_levels,
                    g_scaleprof.max_level_wall_ns * 1e-6, g_scaleprof.max_level_cnt, g_scaleprof.max_level_nc,
                    tot > 0 ? 100.0 * setupw / tot : 0.0, tot > 0 ? 100.0 * nw / tot : 0.0);
    }
#endif
#ifdef CRD_HESAP_CHOL_PROFILE
    {
        const double sc = g_cholprof.scatter_ns * 1e-6;
        const double cm = g_cholprof.cmod_ns * 1e-6;
        const double cd = g_cholprof.cdiv_ns * 1e-6;
        const double cm_gf =
            g_cholprof.cmod_ns > 0 ? static_cast<double>(g_cholprof.cmod_flops) / g_cholprof.cmod_ns : 0.0;
        const double cd_gf =
            g_cholprof.cdiv_ns > 0 ? static_cast<double>(g_cholprof.cdiv_flops) / g_cholprof.cdiv_ns : 0.0;
        const double flop_per_call =
            g_cholprof.cmod_calls > 0 ? static_cast<double>(g_cholprof.cmod_flops) / g_cholprof.cmod_calls : 0.0;
        std::printf("  [CHOLPROF n=%u ns=%u] scatter=%.1fms cmod=%.1fms(%.1f GF/s) cdiv=%.1fms(%.1f GF/s) "
                    "| cmod_flop=%.2fe9 cdiv_flop=%.2fe9 | cmod_calls=%llu flop/call=%.0f\n",
                    m_n, ns, sc, cm, cm_gf, cd, cd_gf, static_cast<double>(g_cholprof.cmod_flops) * 1e-9,
                    static_cast<double>(g_cholprof.cdiv_flops) * 1e-9,
                    static_cast<unsigned long long>(g_cholprof.cmod_calls), flop_per_call);
        const char* binlbl[CholProf::kBins] = {"<8", "8-15", "16-31", "32-63", "64-127", "128+"};
        std::printf("  [CHOLPROF-KBIN knc:");
        for (int bn = 0; bn < CholProf::kBins; ++bn)
        {
            const double pct = g_cholprof.cmod_flops > 0 ? 100.0 * static_cast<double>(g_cholprof.cmod_flop_bin[bn]) /
                                                               static_cast<double>(g_cholprof.cmod_flops)
                                                         : 0.0;
            const double gf = g_cholprof.cmod_ns_bin[bn] > 0
                                  ? static_cast<double>(g_cholprof.cmod_flop_bin[bn]) / g_cholprof.cmod_ns_bin[bn]
                                  : 0.0;
            std::printf(" %s=%.0f%%/%.0fGF", binlbl[bn], pct, gf);
        }
        std::printf("]\n");
        // cmod internal split: where does the cmod time actually go — the gemm, the
        // rr[]-indexed scatter-subtract of U into the panel, or per-descendant overhead
        // (binary search + loop control)? Decides whether the next lever is the gemm
        // kernel (already at the square ceiling) or fusing the scatter / cutting overhead.
        double cmod_gemm_ms = 0.0;
        for (int bn = 0; bn < CholProf::kBins; ++bn)
        {
            cmod_gemm_ms += g_cholprof.cmod_ns_bin[bn] * 1e-6;
        }
        const double cmod_scat_ms = g_cholprof.cmod_scatter_ns * 1e-6;
        std::printf("  [CHOLPROF-CMOD cmod=%.1fms = gemm=%.1fms + scatter_sub=%.1fms + overhead=%.1fms]\n", cm,
                    cmod_gemm_ms, cmod_scat_ms, cm - cmod_gemm_ms - cmod_scat_ms);
        // The K≥128 calls (the 52-GF/s wall) by output width m1: small m1 ⇒ skinny-C ⇒
        // memory-bound ⇒ widen-C (amalgamation) is the lever, not the kernel.
        const char* m1lbl[CholProf::kM1Bins] = {"<16", "16-63", "64-255", "256-1023", "1024+"};
        crd::u64 hk_total = 0;
        for (int bn = 0; bn < CholProf::kM1Bins; ++bn)
        {
            hk_total += g_cholprof.cmod_hk_m1_flop[bn];
        }
        std::printf("  [CHOLPROF-M1 (K>=128) m1:");
        for (int bn = 0; bn < CholProf::kM1Bins; ++bn)
        {
            const double pct = hk_total > 0 ? 100.0 * static_cast<double>(g_cholprof.cmod_hk_m1_flop[bn]) /
                                                  static_cast<double>(hk_total)
                                            : 0.0;
            std::printf(" %s=%.0f%%(%lluc)", m1lbl[bn], pct,
                        static_cast<unsigned long long>(g_cholprof.cmod_hk_m1_count[bn]));
        }
        std::printf("]\n");
        // cdiv (25 GF/s — worst phase) by panel width nc: where is the cdiv time?
        const char* cdlbl[CholProf::kCdivBins] = {"<=4", "5-32", "33-127", "128-511", "512+"};
        std::printf("  [CHOLPROF-CDIV nc:");
        for (int bn = 0; bn < CholProf::kCdivBins; ++bn)
        {
            const double pct = g_cholprof.cdiv_flops > 0 ? 100.0 * static_cast<double>(g_cholprof.cdiv_flop_bin[bn]) /
                                                               static_cast<double>(g_cholprof.cdiv_flops)
                                                         : 0.0;
            const double gf = g_cholprof.cdiv_ns_bin[bn] > 0
                                  ? static_cast<double>(g_cholprof.cdiv_flop_bin[bn]) / g_cholprof.cdiv_ns_bin[bn]
                                  : 0.0;
            const double ms = g_cholprof.cdiv_ns_bin[bn] * 1e-6;
            std::printf(" %s=%.0f%%/%.0fGF/%.0fms", cdlbl[bn], pct, gf, ms);
        }
        std::printf("]\n");
        // Within-cdiv split: OUTER trailing (B, K=256 bulk gemm) vs the rest (A1 scalar POTF2 +
        // A2 below-solve + A3 inner trailing, all K=64). Decides which cdiv sub-lever to cut.
        const double ot_ms = g_cholprof.cdiv_outertrail_ns * 1e-6;
        std::printf("  [CHOLPROF-CDIV-SPLIT cdiv=%.1fms = outer_trail(B,K=256)=%.1fms + within_panel(A,K=64)=%.1fms]\n",
                    cd, ot_ms, cd - ot_ms);
    }
#endif
}

template <typename T> bool SupernodalCholesky<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    // v5a-5: small single-RHS => serial (the fast dedicated hand path); large => level-parallel. Multi-RHS
    // always parallelizes (block-gemm wins at every size). solve_with_workers still honors nw, so the
    // determinism moat test can force the parallel path on any matrix regardless of this gate.
    const crd::u32 nw = (nrhs == 1 && m_lnz < kSolveParallelMinLnz) ? 1U : crd::jobs::num_workers();
    return solve_with_workers(rhs, nrhs, nw);
}

template <typename T>
bool SupernodalCholesky<T>::solve_with_workers(crd::containers::Span<T> rhs, crd::usize nrhs, crd::u32 nw) const
{
    if (m_info != 0)
    {
        return false;
    }
    const SupernodalSymbolic& sym = m_sym;
    const crd::u32 ns = sym.nsuper;
    if (ns == 0)
    {
        return true;
    }
    crd::u32 max_below = 1;
    crd::u32 max_nc = 1;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        const crd::u32 ncs = sym.scol[s + 1] - sym.scol[s];
        const crd::u32 bw = (sym.srowp[s + 1] - sym.srowp[s]) - ncs;
        max_below = bw > max_below ? bw : max_below;
        max_nc = ncs > max_nc ? ncs : max_nc;
    }
    // Multi-RHS diagonal-block solve goes BATCHED (c-contiguous scratch, L_diag read once) above this
    // width; below it the per-RHS path avoids the copy overhead (bmwcra's ~12k tiny nc=2-4 fronts).
    constexpr crd::u32 solve_batch_min_nc = 48;
    if (nrhs == 1 && nw <= 1)
    {
        // Single-RHS, SERIAL (nw<=1): ColMajor hand-axpy. At the latency-bound frontier; a block
        // gemm's small-K overhead would regress this (the failed-gemv lesson), so serial single-RHS
        // keeps the hand path. v5a-5: at nw>1 single-RHS instead takes the level-parallel hand path
        // in fwd_one/back_one below (same hand-axpy kernel + k-ascending reduction order => bit-
        // identical to this), so the determinism moat holds across worker counts.
        crd::containers::Array<T> tmp(m_alloc);
        tmp.resize(max_below);
        T* x = rhs.data();
        for (crd::u32 s = 0; s < ns; ++s) // forward L·y = b
        {
            const crd::u32 rb = sym.srowp[s];
            const crd::u32 nr = sym.srowp[s + 1] - rb;
            const crd::u32 firstcol = sym.scol[s];
            const crd::u32 nc = sym.scol[s + 1] - firstcol;
            const T* panel = &m_lx[m_lxp[s]];
            for (crd::u32 j = 0; j < nc; ++j) // right-looking: contiguous column axpy on the ColMajor panel
            {
                const T yj = x[firstcol + j] / panel[static_cast<crd::usize>(j) * nr + j];
                x[firstcol + j] = yj;
                const T* colj = panel + static_cast<crd::usize>(j) * nr;
                for (crd::u32 i = j + 1; i < nc; ++i)
                {
                    x[firstcol + i] -= colj[i] * yj; // L[i,j] unit-stride in i — vectorizes
                }
            }
            const crd::u32 below = nr - nc;
            if (below > 0)
            {
                for (crd::u32 r = 0; r < below; ++r)
                {
                    tmp[r] = T{0};
                }
                for (crd::u32 k = 0; k < nc; ++k)
                {
                    const T yk = x[firstcol + k];
                    const T* colk = panel + static_cast<crd::usize>(k) * nr + nc;
                    for (crd::u32 r = 0; r < below; ++r)
                    {
                        tmp[r] += colk[r] * yk;
                    }
                }
                for (crd::u32 r = 0; r < below; ++r)
                {
                    x[sym.srow[rb + nc + r]] -= tmp[r];
                }
            }
        }
        for (crd::u32 si = ns; si-- > 0;) // backward Lᴴ·x = y (Lᵀ for real)
        {
            const crd::u32 rb = sym.srowp[si];
            const crd::u32 nr = sym.srowp[si + 1] - rb;
            const crd::u32 firstcol = sym.scol[si];
            const crd::u32 nc = sym.scol[si + 1] - firstcol;
            const T* panel = &m_lx[m_lxp[si]];
            const crd::u32 below = nr - nc;
            if (below > 0)
            {
                for (crd::u32 r = 0; r < below; ++r)
                {
                    tmp[r] = x[sym.srow[rb + nc + r]];
                }
                for (crd::u32 k = 0; k < nc; ++k)
                {
                    const T* colk = panel + static_cast<crd::usize>(k) * nr + nc;
                    T acc = T{0};
                    for (crd::u32 r = 0; r < below; ++r)
                    {
                        acc += chol_conj<T>(colk[r]) * tmp[r]; // Lᴴ entry = conj(L)
                    }
                    x[firstcol + k] -= acc;
                }
            }
            for (crd::u32 jj = nc; jj-- > 0;)
            {
                const T* coljj = panel + static_cast<crd::usize>(jj) * nr;
                T v = x[firstcol + jj];
                for (crd::u32 k = jj + 1; k < nc; ++k)
                {
                    v -= chol_conj<T>(coljj[k]) * x[firstcol + k]; // Lᴴ entry = conj(L)
                }
                x[firstcol + jj] = v / coljj[jj]; // coljj[jj] = L[jj][jj] is real
            }
        }
        return true;
    }

    // Multi-RHS: column-major n × nrhs, ld = m_n. `nw` (caller-forced, = num_workers() in the public solve)
    // selects the path: nw>1 ⇒ level-PARALLEL race-free LEFT-looking (each supernode writes only its own
    // columns — the forward gathers from descendants ascending, the backward from ancestors descending);
    // nw≤1 ⇒ SERIAL RIGHT-looking forward (few big gemms — ~15-20% faster than the left-looking gather's
    // many tiny gemms at 1T, bench-confirmed). Both forwards share the k-ascending accumulation order ⇒
    // bit-identical x. Concurrent same-level supernodes need DISJOINT scratch ⇒ per-worker tmp/dscr slices,
    // SIZED BY THE POOL (not nw): parallel_for(cnt, nw) dispatches onto the shared pool, so worker_index()
    // is POOL-GLOBAL (0..num_workers()-1) regardless of nw — sizing by nw heap-overflows for 1<nw<pool.
    const crd::u32 sw = nw > 1 ? crd::jobs::num_workers() : 1;
    crd::containers::Array<T> tmp(m_alloc); // [sw] × (below × nrhs) work block (ColMajor, ld = below)
    tmp.resize(static_cast<crd::usize>(sw) * max_below * nrhs);
    crd::containers::Array<T> dscr(m_alloc); // [sw] × (nc × nrhs) RowMajor diag scratch — batched diag solve
    dscr.resize(static_cast<crd::usize>(sw) * max_nc * nrhs);
    T* xb = rhs.data();
    const crd::usize ldx = m_n;
    // Forward L·Y = B — LEFT-LOOKING (race-free, level-parallel like the backward). Each supernode s
    // GATHERS Σ_{k ∈ upd_list[s]} L_{s,k}·Y_k from its already-solved descendants and writes ONLY its own
    // columns (vs the right-looking scatter into shared ancestor rows, which races same-level supernodes).
    // L_{s,k} is exactly descendant k's panel rows that fall in s's column range [firstcol, firstcol+nc)
    // — a contiguous sub-block found by binary search (the cmod gather mirror) — and k-ascending upd_list
    // order == the right-looking accumulation order ⇒ bit-identical to the serial right-looking forward
    // (moat + residual hold). No subdiagonal scatter: each ancestor gathers s's contribution in its turn.
    auto fwd_one = [&](crd::u32 s, crd::u32 worker)
    {
        T* wdscr = dscr.data() + static_cast<crd::usize>(worker) * max_nc * nrhs; // gather C + diag scratch
        const crd::u32 rb = sym.srowp[s];
        const crd::u32 nr = sym.srowp[s + 1] - rb;
        const crd::u32 firstcol = sym.scol[s];
        const crd::u32 nc = sym.scol[s + 1] - firstcol;
        const T* panel = &m_lx[m_lxp[s]];
        // v5a-5: single-RHS takes the hand-axpy LEFT-looking gather + right-looking diagonal solve below
        // (no N=1 gemm — the failed-gemv lesson). BIT-IDENTICAL to the dedicated nw<=1 forward: each
        // descendant's contribution is grouped and summed over that descendant's columns ascending, and
        // descendants are visited in k-ascending upd_list order == the dedicated right-looking order.
        if (nrhs == 1)
        {
            for (crd::u32 ui = m_upd_ptr[s]; ui < m_upd_ptr[s + 1]; ++ui)
            {
                const crd::u32 k = m_upd_list[ui];
                const crd::u32 krb = sym.srowp[k];
                const crd::u32 knr = sym.srowp[k + 1] - krb;
                const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
                const crd::u32 kfirstcol = sym.scol[k];
                const T* kpanel = &m_lx[m_lxp[k]];
                crd::u32 lo = knc;
                crd::u32 hi = knr;
                while (lo < hi)
                {
                    const crd::u32 mid = lo + (hi - lo) / 2;
                    if (sym.srow[krb + mid] < firstcol)
                    {
                        lo = mid + 1;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                const crd::u32 p0 = lo;
                crd::u32 m1 = 0;
                while (p0 + m1 < knr && sym.srow[krb + p0 + m1] < firstcol + nc)
                {
                    ++m1;
                }
                if (m1 == 0)
                {
                    continue;
                }
                for (crd::u32 i = 0; i < m1; ++i)
                {
                    wdscr[i] = T{0};
                }
                for (crd::u32 j = 0; j < knc; ++j) // sum descendant k's columns ascending (== dedicated)
                {
                    const T yj = xb[kfirstcol + j];
                    const T* col = kpanel + static_cast<crd::usize>(j) * knr + p0;
                    for (crd::u32 i = 0; i < m1; ++i)
                    {
                        wdscr[i] += col[i] * yj;
                    }
                }
                for (crd::u32 i = 0; i < m1; ++i)
                {
                    xb[sym.srow[krb + p0 + i]] -= wdscr[i];
                }
            }
            for (crd::u32 j = 0; j < nc; ++j) // diagonal forward solve: right-looking contiguous column axpy
            {
                const T yj = xb[firstcol + j] / panel[static_cast<crd::usize>(j) * nr + j];
                xb[firstcol + j] = yj;
                const T* colj = panel + static_cast<crd::usize>(j) * nr;
                for (crd::u32 i = j + 1; i < nc; ++i)
                {
                    xb[firstcol + i] -= colj[i] * yj;
                }
            }
            return;
        }
        for (crd::u32 ui = m_upd_ptr[s]; ui < m_upd_ptr[s + 1]; ++ui) // gather from descendants (k-ascending)
        {
            const crd::u32 k = m_upd_list[ui];
            const crd::u32 krb = sym.srowp[k];
            const crd::u32 knr = sym.srowp[k + 1] - krb;
            const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
            const crd::u32 kfirstcol = sym.scol[k];
            const T* kpanel = &m_lx[m_lxp[k]];
            crd::u32 lo = knc; // p0 = first k-row ≥ firstcol (k's dense diagonal block is rows [0,knc))
            crd::u32 hi = knr;
            while (lo < hi)
            {
                const crd::u32 mid = lo + (hi - lo) / 2;
                if (sym.srow[krb + mid] < firstcol)
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }
            const crd::u32 p0 = lo;
            crd::u32 m1 = 0; // # of k-rows in [firstcol, firstcol+nc) = the rows L_{s,k} touches
            while (p0 + m1 < knr && sym.srow[krb + p0 + m1] < firstcol + nc)
            {
                ++m1;
            }
            if (m1 == 0)
            {
                continue;
            }
            // C(m1×nrhs, ColMajor ld=m1) = kpanel[p0.., :knc] (m1×knc) · Y_k(knc×nrhs). Forward uses L
            // (NOT Lᴴ — the conj is the backward's); here None/None. wdscr holds C (consumed before diag).
            const dense::MatrixView<const T, dense::Layout::ColMajor> akv(kpanel + p0, m1, knc, knr);
            const dense::MatrixView<const T, dense::Layout::ColMajor> ykv(xb + kfirstcol, knc, nrhs, ldx);
            const dense::MatrixView<T, dense::Layout::ColMajor> cv(wdscr, m1, nrhs, m1);
            dense::gemm<T, dense::Layout::ColMajor>(T{1}, akv, ykv, T{0}, cv, dense::Trans::None, dense::Trans::None,
                                                    nullptr);
            for (crd::usize c = 0; c < nrhs; ++c) // B_s -= C at global rows srow[k][p0+i]
            {
                T* xc = xb + c * ldx;
                const T* cc = wdscr + c * static_cast<crd::usize>(m1);
                for (crd::u32 i = 0; i < m1; ++i)
                {
                    xc[sym.srow[krb + p0 + i]] -= cc[i];
                }
            }
        }
        if (nc >= solve_batch_min_nc)
        {
            // Batched diagonal forward solve (wide fronts): c-contiguous scratch, all nrhs per L-element
            // (reads L_diag ONCE vs nrhs×; inner c-loop vectorizes), DIVIDE (not ×reciprocal) ⇒ same
            // per-(i,c) op sequence as the per-RHS path ⇒ bit-identical (moat/residual unchanged).
            for (crd::u32 j = 0; j < nc; ++j)
            {
                T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                const T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    dj[c] = xj[c * ldx];
                }
            }
            for (crd::u32 j = 0; j < nc; ++j)
            {
                const T ljj = panel[static_cast<crd::usize>(j) * nr + j];
                T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    dj[c] = dj[c] / ljj;
                }
                const T* colj = panel + static_cast<crd::usize>(j) * nr;
                for (crd::u32 i = j + 1; i < nc; ++i)
                {
                    const T lij = colj[i];
                    T* di = wdscr + static_cast<crd::usize>(i) * nrhs;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        di[c] -= lij * dj[c];
                    }
                }
            }
            for (crd::u32 j = 0; j < nc; ++j)
            {
                const T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    xj[c * ldx] = dj[c];
                }
            }
        }
        else
        {
            for (crd::usize c = 0; c < nrhs; ++c) // narrow fronts: per-RHS (avoids the copy overhead)
            {
                T* xc = xb + c * ldx;
                for (crd::u32 j = 0; j < nc; ++j) // contiguous column axpy on the ColMajor panel
                {
                    const T yj = xc[firstcol + j] / panel[static_cast<crd::usize>(j) * nr + j];
                    xc[firstcol + j] = yj;
                    const T* colj = panel + static_cast<crd::usize>(j) * nr;
                    for (crd::u32 i = j + 1; i < nc; ++i)
                    {
                        xc[firstcol + i] -= colj[i] * yj; // L[i,j] unit-stride in i — vectorizes
                    }
                }
            }
        }
    };
    if (nw > 1) // level-parallel forward: levels ASCENDING (leaves first), same-level concurrent
    {
        for (crd::u32 li = 0; li < m_nlevels; ++li)
        {
            const crd::u32 off = m_lvl_ptr[li];
            const crd::u32 cnt = m_lvl_ptr[li + 1] - off;
            if (cnt >= 2)
            {
                auto* counter = crd::jobs::parallel_for(cnt, nw,
                                                        [&, off](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 w = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                fwd_one(m_lvl_list[off + t], w);
                                                            }
                                                        });
                crd::jobs::wait(counter);
                crd::jobs::frame_reset();
            }
            else // thin level (near-root big front): serial, no spawn
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    fwd_one(m_lvl_list[off + t], 0);
                }
            }
        }
    }
    else
    {
        // SERIAL forward: RIGHT-looking (few big subdiagonal gemms — ~15-20% faster than the left-looking
        // gather's many tiny per-descendant gemms at 1T). Each supernode solves its diagonal then ONE gemm
        // scatters L_below·Y_s into ancestor rows. Bit-identical to the parallel left-looking path: both
        // accumulate descendant contributions into B_s in k-ascending order before the diagonal solve, and
        // the per-row gemm values are layout-independent (the solve determinism moat). Scratch slice 0.
        for (crd::u32 s = 0; s < ns; ++s)
        {
            const crd::u32 rb = sym.srowp[s];
            const crd::u32 nr = sym.srowp[s + 1] - rb;
            const crd::u32 firstcol = sym.scol[s];
            const crd::u32 nc = sym.scol[s + 1] - firstcol;
            const T* panel = &m_lx[m_lxp[s]];
            if (nc >= solve_batch_min_nc)
            {
                for (crd::u32 j = 0; j < nc; ++j) // batched diag (c-contiguous scratch, DIVIDE)
                {
                    T* dj = dscr.data() + static_cast<crd::usize>(j) * nrhs;
                    const T* xj = xb + firstcol + j;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        dj[c] = xj[c * ldx];
                    }
                }
                for (crd::u32 j = 0; j < nc; ++j)
                {
                    const T ljj = panel[static_cast<crd::usize>(j) * nr + j];
                    T* dj = dscr.data() + static_cast<crd::usize>(j) * nrhs;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        dj[c] = dj[c] / ljj;
                    }
                    const T* colj = panel + static_cast<crd::usize>(j) * nr;
                    for (crd::u32 i = j + 1; i < nc; ++i)
                    {
                        const T lij = colj[i];
                        T* di = dscr.data() + static_cast<crd::usize>(i) * nrhs;
                        for (crd::usize c = 0; c < nrhs; ++c)
                        {
                            di[c] -= lij * dj[c];
                        }
                    }
                }
                for (crd::u32 j = 0; j < nc; ++j)
                {
                    const T* dj = dscr.data() + static_cast<crd::usize>(j) * nrhs;
                    T* xj = xb + firstcol + j;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        xj[c * ldx] = dj[c];
                    }
                }
            }
            else
            {
                for (crd::usize c = 0; c < nrhs; ++c) // narrow fronts: per-RHS
                {
                    T* xc = xb + c * ldx;
                    for (crd::u32 j = 0; j < nc; ++j)
                    {
                        const T yj = xc[firstcol + j] / panel[static_cast<crd::usize>(j) * nr + j];
                        xc[firstcol + j] = yj;
                        const T* colj = panel + static_cast<crd::usize>(j) * nr;
                        for (crd::u32 i = j + 1; i < nc; ++i)
                        {
                            xc[firstcol + i] -= colj[i] * yj;
                        }
                    }
                }
            }
            const crd::u32 below = nr - nc;
            if (below > 0)
            {
                const dense::MatrixView<const T, dense::Layout::ColMajor> lb(panel + nc, below, nc, nr);
                const dense::MatrixView<const T, dense::Layout::ColMajor> yt(xb + firstcol, nc, nrhs, ldx);
                const dense::MatrixView<T, dense::Layout::ColMajor> tm(tmp.data(), below, nrhs, below);
                dense::gemm<T, dense::Layout::ColMajor>(T{1}, lb, yt, T{0}, tm, dense::Trans::None, dense::Trans::None,
                                                        nullptr);
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    T* xc = xb + c * ldx;
                    const T* tc = tmp.data() + c * static_cast<crd::usize>(below);
                    for (crd::u32 r = 0; r < below; ++r)
                    {
                        xc[sym.srow[rb + nc + r]] -= tc[r];
                    }
                }
            }
        }
    }
    // Backward Lᴴ·X = Y (Lᵀ for real). LEFT-LOOKING: each supernode reads completed ANCESTORS (its own
    // panel below-rows × already-solved x) and writes ONLY its own columns ⇒ level-parallel is race-free
    // + bit-identical across worker counts (per-supernode op order fixed; same-level supernodes
    // independent). Per-worker tmp/dscr slices avoid scratch races.
    auto back_one = [&](crd::u32 si, crd::u32 worker)
    {
        T* wtmp = tmp.data() + static_cast<crd::usize>(worker) * max_below * nrhs;
        T* wdscr = dscr.data() + static_cast<crd::usize>(worker) * max_nc * nrhs;
        const crd::u32 rb = sym.srowp[si];
        const crd::u32 nr = sym.srowp[si + 1] - rb;
        const crd::u32 firstcol = sym.scol[si];
        const crd::u32 nc = sym.scol[si + 1] - firstcol;
        const T* panel = &m_lx[m_lxp[si]];
        const crd::u32 below = nr - nc;
        // v5a-5: single-RHS hand path (no N=1 gemm). BIT-IDENTICAL to the dedicated nw<=1 backward:
        // the below-block gather sums over below-rows ascending, then the descending-jj diagonal back-solve.
        if (nrhs == 1)
        {
            if (below > 0)
            {
                for (crd::u32 k = 0; k < nc; ++k)
                {
                    const T* colk = panel + static_cast<crd::usize>(k) * nr + nc;
                    T acc = T{0};
                    for (crd::u32 r = 0; r < below; ++r)
                    {
                        acc += chol_conj<T>(colk[r]) * xb[sym.srow[rb + nc + r]]; // L^H entry = conj(L)
                    }
                    xb[firstcol + k] -= acc;
                }
            }
            for (crd::u32 jj = nc; jj-- > 0;)
            {
                const T* coljj = panel + static_cast<crd::usize>(jj) * nr;
                T v = xb[firstcol + jj];
                for (crd::u32 k = jj + 1; k < nc; ++k)
                {
                    v -= chol_conj<T>(coljj[k]) * xb[firstcol + k]; // L^H entry = conj(L)
                }
                xb[firstcol + jj] = v / coljj[jj]; // coljj[jj] = L[jj][jj] is real
            }
            return;
        }
        if (below > 0)
        {
            for (crd::usize c = 0; c < nrhs; ++c) // gather below rows
            {
                const T* xc = xb + c * ldx;
                T* tc = wtmp + c * static_cast<crd::usize>(below);
                for (crd::u32 r = 0; r < below; ++r)
                {
                    tc[r] = xc[sym.srow[rb + nc + r]];
                }
            }
            const dense::MatrixView<const T, dense::Layout::ColMajor> lb(panel + nc, below, nc, nr);
            const dense::MatrixView<const T, dense::Layout::ColMajor> tm(wtmp, below, nrhs, below);
            const dense::MatrixView<T, dense::Layout::ColMajor> yt(xb + firstcol, nc, nrhs, ldx);
            // Y -= Lᴴ_below · gathered (ConjTranspose for complex Hermitian; Lᵀ for real).
            dense::gemm<T, dense::Layout::ColMajor>(T{-1}, lb, tm, T{1}, yt, kCholAdjoint<T>, dense::Trans::None,
                                                    nullptr);
        }
        if (nc >= solve_batch_min_nc)
        {
            // Batched diagonal back-solve (Lᴴ upper): c-contiguous scratch, all nrhs per L-element,
            // DIVIDE for bit-identity. Same descending-jj / ascending-k op order as the per-RHS path.
            for (crd::u32 j = 0; j < nc; ++j)
            {
                T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                const T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    dj[c] = xj[c * ldx];
                }
            }
            for (crd::u32 jj = nc; jj-- > 0;)
            {
                const T* coljj = panel + static_cast<crd::usize>(jj) * nr;
                T* djj = wdscr + static_cast<crd::usize>(jj) * nrhs;
                for (crd::u32 k = jj + 1; k < nc; ++k)
                {
                    const T lkk = chol_conj<T>(coljj[k]); // Lᴴ entry = conj(L)
                    const T* dk = wdscr + static_cast<crd::usize>(k) * nrhs;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        djj[c] -= lkk * dk[c];
                    }
                }
                const T ljj = coljj[jj]; // L[jj][jj] is real
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    djj[c] = djj[c] / ljj;
                }
            }
            for (crd::u32 j = 0; j < nc; ++j)
            {
                const T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    xj[c * ldx] = dj[c];
                }
            }
        }
        else
        {
            for (crd::usize c = 0; c < nrhs; ++c) // narrow fronts: per-RHS
            {
                T* xc = xb + c * ldx;
                for (crd::u32 jj = nc; jj-- > 0;)
                {
                    const T* coljj = panel + static_cast<crd::usize>(jj) * nr;
                    T v = xc[firstcol + jj];
                    for (crd::u32 k = jj + 1; k < nc; ++k)
                    {
                        v -= chol_conj<T>(coljj[k]) * xc[firstcol + k]; // Lᴴ entry = conj(L)
                    }
                    xc[firstcol + jj] = v / coljj[jj]; // coljj[jj] = L[jj][jj] is real
                }
            }
        }
    };
    if (nw > 1) // level-parallel backward: levels DESCENDING (root level first), same-level concurrent
    {
        for (crd::u32 li = m_nlevels; li-- > 0;)
        {
            const crd::u32 off = m_lvl_ptr[li];
            const crd::u32 cnt = m_lvl_ptr[li + 1] - off;
            if (cnt >= 2)
            {
                auto* counter = crd::jobs::parallel_for(cnt, nw,
                                                        [&, off](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 w = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                back_one(m_lvl_list[off + t], w);
                                                            }
                                                        });
                crd::jobs::wait(counter);
                crd::jobs::frame_reset();
            }
            else // thin level (near-root big front): serial, no spawn
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    back_one(m_lvl_list[off + t], 0);
                }
            }
        }
    }
    else
    {
        for (crd::u32 si = ns; si-- > 0;) // serial: reverse supernode order (valid topological order)
        {
            back_one(si, 0);
        }
    }
    return true;
}

template <typename T>
SupernodalCholesky<T> factor_supernodal_cholesky(const sparse::SparsePattern& pattern,
                                                 crd::containers::ConstSpan<T> values, crd::memory::IAllocator* alloc,
                                                 crd::u32 nrelax, crd::u32 num_workers)
{
    SupernodalCholesky<T> f(alloc);
    f.factorize(pattern, values, nrelax, num_workers);
    return f;
}

template class SupernodalCholesky<crd::f32>;
template class SupernodalCholesky<crd::f64>;
template class SupernodalCholesky<Complex32>; // complex Hermitian LLᴴ (v5a-2)
template class SupernodalCholesky<Complex64>;
template SupernodalCholesky<crd::f32> factor_supernodal_cholesky<crd::f32>(const sparse::SparsePattern&,
                                                                           crd::containers::ConstSpan<crd::f32>,
                                                                           crd::memory::IAllocator*, crd::u32,
                                                                           crd::u32);
template SupernodalCholesky<crd::f64> factor_supernodal_cholesky<crd::f64>(const sparse::SparsePattern&,
                                                                           crd::containers::ConstSpan<crd::f64>,
                                                                           crd::memory::IAllocator*, crd::u32,
                                                                           crd::u32);
template SupernodalCholesky<Complex32> factor_supernodal_cholesky<Complex32>(const sparse::SparsePattern&,
                                                                             crd::containers::ConstSpan<Complex32>,
                                                                             crd::memory::IAllocator*, crd::u32,
                                                                             crd::u32);
template SupernodalCholesky<Complex64> factor_supernodal_cholesky<Complex64>(const sparse::SparsePattern&,
                                                                             crd::containers::ConstSpan<Complex64>,
                                                                             crd::memory::IAllocator*, crd::u32,
                                                                             crd::u32);

} // namespace crd::hesap::direct
