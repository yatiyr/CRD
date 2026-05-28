// bench_hesap_ordering_vs_reference -- Phase 3.1.6 v2a.
//
// On real SuiteSparse SPD matrices (bcsstk* FEM):
//   (1) VALIDATE the symbolic-Cholesky port: our nnz_l(natural) MUST equal
//       Eigen SimplicialLLT<Lower, NaturalOrdering>.matrixL().nonZeros().
//   (2) RCM fill/bandwidth reduction (our nnz_l + bandwidth, natural vs RCM).
//   (3) Set the v2b AMD target: Eigen SimplicialLLT<Lower, AMDOrdering> nnz(L)
//       — the number our AMD (v2b) must reach (≤ 1.05×) + beat on time.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named GrowableTlsfAllocator (never malloc / default_allocator); crd::f64/usize/
// i32/i64 (never raw). Raw double only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/OrderingMethods>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <chrono>
#include <cstdio>
#include <cstdlib> // strtol / strtod
#include <cstring> // strstr

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;
namespace fs = crd::platform::fs;

// Unbounded pooled arena (handles any matrix without a per-size pool guess).
crd::memory::GrowableTlsfAllocator g_alloc;

struct Trip
{
    crd::i32 r, c;
    crd::f64 v;
};

// Matrix-Market reader, crd-native: read the file into a crd::String via the platform
// filesystem, hand-parse with strtol/strtod (no std::ifstream/string/sstream). Handles
// MatrixMarket banner flags pattern / symmetric / hermitian. DATA → crd::Array<Trip>.
bool read_mtx(const char* path, crd::i32& n, crd::containers::Array<Trip>& trips)
{
    crd::containers::String text(&g_alloc);
    if (!fs::read_file_text(fs::Path{path}, text))
    {
        return false;
    }
    const char* p = text.c_str();
    const char* end = p + text.size();
    bool is_pattern = false;
    bool symmetric = false;
    bool dims_read = false;
    crd::i32 nnz = 0;
    crd::i32 seen = 0;
    while (p < end)
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        {
            ++p;
        }
        if (p >= end)
        {
            break;
        }
        if (*p == '%') // banner / comment line
        {
            const char* eol = p;
            while (eol < end && *eol != '\n')
            {
                ++eol;
            }
            if (!dims_read)
            {
                // Scan the (NUL-terminated) banner segment for the MatrixMarket flags.
                for (const char* q = p; q + 9 <= eol; ++q)
                {
                    if (std::strncmp(q, "pattern", 7) == 0)
                    {
                        is_pattern = true;
                    }
                    if (std::strncmp(q, "symmetric", 9) == 0 || std::strncmp(q, "hermitian", 9) == 0)
                    {
                        symmetric = true;
                    }
                }
            }
            p = eol;
            continue;
        }
        char* np = nullptr;
        if (!dims_read)
        {
            const crd::i32 rows = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            const crd::i32 cols = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            nnz = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            n = rows > cols ? rows : cols;
            trips.reserve(static_cast<crd::usize>(nnz) * (symmetric ? 2 : 1));
            dims_read = true;
            continue;
        }
        const crd::i32 r = static_cast<crd::i32>(std::strtol(p, &np, 10)) - 1;
        p = np;
        const crd::i32 c = static_cast<crd::i32>(std::strtol(p, &np, 10)) - 1;
        p = np;
        crd::f64 v = 1.0;
        if (!is_pattern)
        {
            v = std::strtod(p, &np);
            p = np;
        }
        trips.push_back(Trip{r, c, v});
        if (symmetric && r != c)
        {
            trips.push_back(Trip{c, r, v});
        }
        if (++seen >= nnz)
        {
            break;
        }
    }
    return dims_read;
}

template <typename Fn> crd::f64 best_ms(Fn&& fn, crd::i32 reps = 5)
{
    fn();
    crd::f64 best = 1e30;
    for (crd::i32 r = 0; r < reps; ++r)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* name)
{
    crd::containers::String path(&g_alloc);
    path.append(CRD_SUITESPARSE_DIR);
    path.append("/");
    path.append(name);
    path.append("/");
    path.append(name);
    path.append(".mtx");
    crd::i32 n = 0;
    crd::containers::Array<Trip> trips(&g_alloc);
    if (!read_mtx(path.c_str(), n, trips))
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }

    crd::memory::IAllocator* alloc = &g_alloc;
    sp::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();

    // Cerid: nnz(L) natural + RCM, bandwidth natural + RCM, RCM time.
    const crd::u64 nnzl_nat = ord::nnz_l(a.pattern(), alloc);
    const crd::u32 bw_nat = ord::bandwidth(a.pattern());
    crd::u64 nnzl_rcm = 0;
    crd::u32 bw_rcm = 0;
    const crd::f64 t_rcm = best_ms(
        [&]()
        {
            auto p = ord::rcm_order(a.pattern(), alloc);
            auto rp = ord::apply_symmetric(a.pattern(), p, alloc);
            nnzl_rcm = ord::nnz_l(rp, alloc);
            bw_rcm = ord::bandwidth(rp);
        });
    (void)t_rcm;

    // Cerid AMD (v2b): ordering time alone, then its fill.
    ord::Permutation amdp(alloc);
    const crd::f64 t_amd = best_ms([&]() { amdp = ord::amd_order(a.pattern(), alloc); });
    const crd::u64 nnzl_amd = ord::nnz_l(ord::apply_symmetric(a.pattern(), amdp, alloc), alloc);

    // Eigen: build symmetric SparseMatrix (lower triangle drives LLT).
    crd::containers::Array<Eigen::Triplet<double>> et(alloc);
    et.reserve(trips.size());
    for (const Trip& t : trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());

    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::NaturalOrdering<int>> llt_nat;
    llt_nat.compute(ea);
    long long eig_nat = -1;
    if (llt_nat.info() == Eigen::Success)
    {
        const Eigen::SparseMatrix<double> l_nat = llt_nat.matrixL(); // materialise TriangularView
        eig_nat = l_nat.nonZeros();
    }

    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::AMDOrdering<int>> llt_amd;
    llt_amd.compute(ea);
    long long eig_amd = -1;
    if (llt_amd.info() == Eigen::Success)
    {
        const Eigen::SparseMatrix<double> l_amd = llt_amd.matrixL();
        eig_amd = l_amd.nonZeros();
    }

    const bool port_ok = (eig_nat < 0) || (static_cast<long long>(nnzl_nat) == eig_nat);
    const crd::f64 amd_ratio = eig_amd > 0 ? static_cast<crd::f64>(nnzl_amd) / static_cast<crd::f64>(eig_amd) : 0.0;
    std::printf("  %-10s n=%-6d | nnz(L) nat ours=%-9llu Eigen=%-9lld %s | RCM=%-9llu bw %u->%u | "
                "Eigen-AMD=%-9lld | OUR AMD=%-9llu (%.3fx Eigen-AMD %s) ord=%.2f ms\n",
                name, n, static_cast<unsigned long long>(nnzl_nat), eig_nat, (port_ok ? "MATCH" : "MISMATCH!"),
                static_cast<unsigned long long>(nnzl_rcm), bw_nat, bw_rcm, eig_amd,
                static_cast<unsigned long long>(nnzl_amd), amd_ratio,
                (amd_ratio > 0.0 && amd_ratio <= 1.05 ? "GATE-OK" : "OVER"), t_amd);

    // --- v2c: full symbolic factorisation -------------------------------
    ord::SymbolicFactor sf(alloc);
    const crd::f64 t_sym_full = best_ms([&]() { sf = ord::symbolic_factorize(a.pattern(), alloc); });
    const crd::f64 t_sym_anal = best_ms([&]() { (void)ord::nnz_l(a.pattern(), alloc); });

    crd::f64 t_eig_anal = 0.0;
    {
        Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::NaturalOrdering<int>> llt_a;
        t_eig_anal = best_ms([&]() { llt_a.analyzePattern(ea); });
    }

    // (2) rigorous PATTERN gate: column-by-column row-index diff of our L against
    //     Eigen's numeric L factor (NaturalOrdering -> directly comparable).
    bool pattern_ok = (eig_nat >= 0) && (static_cast<long long>(sf.nnz()) == eig_nat);
    if (eig_nat >= 0)
    {
        const Eigen::SparseMatrix<double> l_nat = llt_nat.matrixL();
        for (int j = 0; j < n && pattern_ok; ++j)
        {
            crd::usize p = sf.lp[static_cast<crd::usize>(j)];
            const crd::usize end = sf.lp[static_cast<crd::usize>(j) + 1];
            for (Eigen::SparseMatrix<double>::InnerIterator it(l_nat, j); it; ++it)
            {
                if (p >= end || sf.li[p] != static_cast<crd::u32>(it.row()))
                {
                    pattern_ok = false;
                    break;
                }
                ++p;
            }
            if (p != end)
            {
                pattern_ok = false;
            }
        }
    }
    const crd::f64 anal_ratio = t_sym_anal > 0.0 ? t_eig_anal / t_sym_anal : 0.0;
    std::printf("             symbolic: L-pattern vs Eigen %s | nsuper=%u (%.1f cols/snode) | "
                "analyze ours=%.3f ms Eigen=%.3f ms (%.2fx) | full(+Li+snode)=%.3f ms\n",
                (pattern_ok ? "MATCH" : "MISMATCH!"), sf.nsuper,
                sf.nsuper ? static_cast<crd::f64>(n) / static_cast<crd::f64>(sf.nsuper) : 0.0, t_sym_anal, t_eig_anal,
                anal_ratio, t_sym_full);

    // (3) v2e-1 early signal: root-level bisection edge-cut, raw v2d (no FM) vs FM-refined.
    auto wbase = ord::detail::to_weighted(ord::build_adjacency(a.pattern(), alloc), alloc);
    crd::containers::Array<ord::WeightedGraph> nd_levels(alloc);
    crd::containers::Array<crd::containers::Array<crd::u32>> nd_cmaps(alloc);
    ord::detail::coarsen(ord::detail::to_weighted(ord::build_adjacency(a.pattern(), alloc), alloc), nd_levels, nd_cmaps,
                         alloc);
    auto raw_part = ord::detail::bisect_coarsest(nd_levels[nd_levels.size() - 1], alloc);
    for (crd::usize i = nd_cmaps.size(); i-- > 0;)
    {
        raw_part = ord::detail::project_down({raw_part.data(), raw_part.size()},
                                             {nd_cmaps[i].data(), nd_cmaps[i].size()}, alloc);
    }
    const crd::u64 raw_cut = ord::detail::edge_cut(wbase, {raw_part.data(), raw_part.size()});
    auto ref_part = ord::detail::bipartition_refined(
        ord::detail::to_weighted(ord::build_adjacency(a.pattern(), alloc), alloc), alloc);
    const crd::u64 ref_cut = ord::detail::edge_cut(wbase, {ref_part.data(), ref_part.size()});
    crd::u32 p0 = 0;
    for (crd::u32 v = 0; v < ref_part.size(); ++v)
    {
        p0 += (ref_part[v] == 0U) ? 1U : 0U;
    }
    auto gfull = ord::build_adjacency(a.pattern(), alloc);
    auto top_sep = ord::detail::vertex_separator(gfull, {ref_part.data(), ref_part.size()}, alloc);
    std::printf("             nd-bisect(root): raw v2d cut=%llu -> FM-refined cut=%llu (%.1f%% reduction) | "
                "balance %u/%u | top |S|=%zu\n",
                static_cast<unsigned long long>(raw_cut), static_cast<unsigned long long>(ref_cut),
                raw_cut ? 100.0 * (1.0 - static_cast<crd::f64>(ref_cut) / static_cast<crd::f64>(raw_cut)) : 0.0, p0,
                n - static_cast<int>(p0), top_sep.size());

    // (4) THE FILL GATE: nested-dissection nnz(L) vs our AMD vs Eigen-AMD.
    ord::Permutation ndp(alloc);
    const crd::f64 t_nd = best_ms([&]() { ndp = ord::nd_order(a.pattern(), alloc); });
    const crd::u64 nnzl_nd = ord::nnz_l(ord::apply_symmetric(a.pattern(), ndp, alloc), alloc);
    const crd::f64 nd_vs_amd = nnzl_amd > 0 ? static_cast<crd::f64>(nnzl_nd) / static_cast<crd::f64>(nnzl_amd) : 0.0;
    const crd::f64 nd_vs_eig = eig_amd > 0 ? static_cast<crd::f64>(nnzl_nd) / static_cast<crd::f64>(eig_amd) : 0.0;
    std::printf("             ND FILL: nnz(L) nd=%-9llu vs ourAMD=%-9llu (%.3fx %s) vs Eigen-AMD=%-9lld (%.3fx %s) "
                "ord=%.2f ms\n",
                static_cast<unsigned long long>(nnzl_nd), static_cast<unsigned long long>(nnzl_amd), nd_vs_amd,
                (nd_vs_amd <= 1.0 ? "WIN" : "lose"), eig_amd, nd_vs_eig, (nd_vs_eig <= 1.0 ? "WIN" : "lose"), t_nd);
}
} // namespace

int main()
{
    std::printf("[bench_hesap_ordering] symbolic-Cholesky port validation + RCM fill/bandwidth + v2b AMD target "
                "(SuiteSparse SPD).\n");
    std::printf("  NOTE: the 'analyze' column compares our nnz_l (etree+post+counts) to Eigen analyzePattern\n"
                "        (etree+counts+Lp). Eigen defers the L row pattern (Li) to factorize, so this is the\n"
                "        only apples-to-apples symbolic comparator; the v2c deliverable is the FULL\n"
                "        symbolic_factorize (Li + supernodes), reported as full(+Li+snode) with no Eigen twin.\n");
    run("bcsstk13");
    run("bcsstk24");
    run("bcsstk25");
    return 0;
}
