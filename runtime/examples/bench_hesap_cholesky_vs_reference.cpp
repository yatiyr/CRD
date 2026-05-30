// bench_hesap_cholesky_vs_reference -- Phase 3.1.6 v5a-1b step 6 (close gate).
//
// On real SuiteSparse SPD matrices (bcsstk* FEM): factor + solve the SAME
// AMD-ordered matrix with (a) Cerid supernodal Cholesky and (b) Eigen
// SimplicialLLT<Lower, NaturalOrdering> (fed the already-permuted matrix, so
// both factor identical structure → a pure numeric-speed comparison). Reports
// factor-time + solve-time ratios, factor fill, and the true residual.
//
// CLOSE TARGETS (ADR-0065 §27): Cerid factor ≥ 1.5x Eigen SimplicialLLT, solve
// ≥ 1.2x; never regress. (CHOLMOD floor is the WSL oracle, separate.)
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. crd conventions throughout;
// raw double only at the Eigen API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;
namespace dir = crd::hesap::direct;
namespace fs = crd::platform::fs;

crd::memory::GrowableTlsfAllocator g_alloc;

struct Trip
{
    crd::i32 r, c;
    crd::f64 v;
};

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
        if (*p == '%')
        {
            const char* eol = p;
            while (eol < end && *eol != '\n')
            {
                ++eol;
            }
            if (!dims_read)
            {
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

template <typename Fn> crd::f64 best_ms(Fn&& fn, crd::i32 reps = 3)
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
    // CRD_BENCH_ONLY=<substr>[,<substr>...] — run only matching matrices (cheap sweeps).
    if (const char* only = std::getenv("CRD_BENCH_ONLY"))
    {
        if (std::strstr(only, name) == nullptr)
        {
            return;
        }
    }
    // CRD_NRELAX overrides the amalgamation relaxation (v5a-4 cmod-flop-rate sweep).
    crd::u32 nrelax = dir::kSupernodeRelax;
    if (const char* nr = std::getenv("CRD_NRELAX"))
    {
        nrelax = static_cast<crd::u32>(std::strtoul(nr, nullptr, 10));
    }
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
    const crd::u32 un = static_cast<crd::u32>(n);

    // Cerid AMD ordering, then permute the matrix (values + pattern) by it so both
    // factorizations see the SAME well-ordered system. new(i,j)=A(perm[i],perm[j]),
    // so original entry (r,c) lands at (inv_perm[r], inv_perm[c]).
    sp::TripletBuilder<crd::f64> tb(&g_alloc, un, un);
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();
    auto perm = ord::amd_order(a.pattern(), &g_alloc);

    sp::TripletBuilder<crd::f64> pb(&g_alloc, un, un);
    pb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        pb.add(perm.inv_perm[static_cast<crd::u32>(t.r)], perm.inv_perm[static_cast<crd::u32>(t.c)], t.v);
    }
    auto ap = pb.compress();
    const crd::containers::ConstSpan<crd::f64> apvals{ap.values().values.data(), ap.values().values.size()};

    // ---- Cerid supernodal Cholesky (TREE-PARALLEL over the worker pool) ----
    const crd::u32 nw = crd::jobs::num_workers();
    const crd::f64 t_cerid_fac = best_ms([&]()
                                         { auto f = dir::factor_supernodal_cholesky<crd::f64>(ap.pattern(), apvals, &g_alloc, nrelax, nw); (void)f.info(); });
    auto cf = dir::factor_supernodal_cholesky<crd::f64>(ap.pattern(), apvals, &g_alloc, nrelax, nw);
    if (std::getenv("CRD_BENCH_SUPERNODE_HIST") != nullptr && cf.info() == 0)
    {
        const auto& sym = cf.symbolic();
        const crd::u32 ns = sym.nsuper;
        crd::u32 buckets[6] = {0, 0, 0, 0, 0, 0}; // nc in 1 / 2-4 / 5-8 / 9-16 / 17-32 / 33+
        crd::u64 cols_small = 0;                   // cols in supernodes with nc<=4
        crd::u64 flop_small = 0, flop_tot = 0;     // solve axpy work ~ nc*below
        crd::u32 maxnc = 0;
        for (crd::u32 s = 0; s < ns; ++s)
        {
            const crd::u32 nc = sym.scol[s + 1] - sym.scol[s];
            const crd::u32 below = (sym.srowp[s + 1] - sym.srowp[s]) - nc;
            maxnc = nc > maxnc ? nc : maxnc;
            const crd::u64 w = static_cast<crd::u64>(nc) * below;
            flop_tot += w;
            if (nc <= 4)
            {
                cols_small += nc;
                flop_small += w;
            }
            const int b = nc == 1 ? 0 : nc <= 4 ? 1 : nc <= 8 ? 2 : nc <= 16 ? 3 : nc <= 32 ? 4 : 5;
            ++buckets[b];
        }
        std::printf("  %-10s HIST nsuper=%u avg_nc=%.1f maxnc=%u | nc[1|2-4|5-8|9-16|17-32|33+]=%u %u %u %u %u %u | "
                    "small(nc<=4): cols=%.1f%% axpy=%.1f%%\n",
                    name, ns, static_cast<double>(n) / ns, maxnc, buckets[0], buckets[1], buckets[2], buckets[3],
                    buckets[4], buckets[5], 100.0 * static_cast<double>(cols_small) / n,
                    flop_tot ? 100.0 * static_cast<double>(flop_small) / static_cast<double>(flop_tot) : 0.0);
    }
    if (cf.info() != 0)
    {
        // Disambiguate: is the matrix genuinely non-PD, or is this our bug? Ask Eigen.
        crd::containers::Array<Eigen::Triplet<double>> et2(&g_alloc);
        et2.reserve(trips.size());
        for (const Trip& t : trips)
        {
            et2.push_back(Eigen::Triplet<double>(static_cast<int>(perm.inv_perm[static_cast<crd::u32>(t.r)]),
                                                 static_cast<int>(perm.inv_perm[static_cast<crd::u32>(t.c)]), t.v));
        }
        Eigen::SparseMatrix<double> ea2(n, n);
        ea2.setFromTriplets(et2.data(), et2.data() + et2.size());
        Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::NaturalOrdering<int>> llt2;
        llt2.compute(ea2);
        std::printf("  %-10s Cerid NOT-SPD (info=%zu); Eigen LLT=%s\n", name, cf.info(),
                    llt2.info() == Eigen::Success ? "SUCCESS => CERID BUG" : "NumericalIssue => matrix not PD");
        return;
    }
    // RHS = A_p * 1; solve; residual.
    crd::containers::Array<crd::f64> b(&g_alloc);
    crd::containers::Array<crd::f64> x(&g_alloc);
    b.resize(un);
    x.resize(un);
    for (crd::u32 i = 0; i < un; ++i)
    {
        b[i] = 0.0;
    }
    for (crd::u32 o = 0; o < un; ++o)
    {
        const crd::u32 st = ap.pattern().outer_ptr[o];
        const crd::u32 cnt = ap.pattern().inner_count(o);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            b[o] += apvals[st + k] * 1.0; // x_true = 1
        }
    }
    const crd::f64 t_cerid_slv = best_ms(
        [&]()
        {
            for (crd::u32 i = 0; i < un; ++i)
            {
                x[i] = b[i];
            }
            (void)cf.solve({x.data(), un});
        });
    crd::f64 cerid_res = 0.0;
    for (crd::u32 i = 0; i < un; ++i)
    {
        cerid_res += (x[i] - 1.0) * (x[i] - 1.0);
    }
    cerid_res = std::sqrt(cerid_res / static_cast<crd::f64>(un));

    // ---- Eigen SimplicialLLT<Lower, NaturalOrdering> on the SAME permuted matrix ----
    // CRD_BENCH_NO_REF skips the (dominant ~39s) Eigen reference entirely — for cdiv/cmod
    // TUNING sweeps that only read Cerid's CHOLPROF profiler. Default OFF ⇒ full shootout.
    const bool no_ref = std::getenv("CRD_BENCH_NO_REF") != nullptr;
    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::NaturalOrdering<int>> llt;
    crd::f64 t_eig_fac = 0.0;
    long long eig_lnz = -1;
    crd::f64 eig_res = -1.0;
    crd::f64 t_eig_slv = 0.0;
    if (!no_ref)
    {
        crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
        et.reserve(trips.size());
        for (const Trip& t : trips)
        {
            const int rr = static_cast<int>(perm.inv_perm[static_cast<crd::u32>(t.r)]);
            const int cc = static_cast<int>(perm.inv_perm[static_cast<crd::u32>(t.c)]);
            et.push_back(Eigen::Triplet<double>(rr, cc, t.v));
        }
        Eigen::SparseMatrix<double> ea(n, n);
        ea.setFromTriplets(et.data(), et.data() + et.size());
        t_eig_fac = best_ms([&]() { llt.compute(ea); });
        if (llt.info() == Eigen::Success)
        {
            const Eigen::SparseMatrix<double> lmat = llt.matrixL();
            eig_lnz = lmat.nonZeros();
            Eigen::VectorXd eb(n);
            for (crd::u32 i = 0; i < un; ++i)
            {
                eb[static_cast<int>(i)] = b[i];
            }
            Eigen::VectorXd ex;
            t_eig_slv = best_ms([&]() { ex = llt.solve(eb); });
            crd::f64 acc = 0.0;
            for (crd::u32 i = 0; i < un; ++i)
            {
                acc += (ex[static_cast<int>(i)] - 1.0) * (ex[static_cast<int>(i)] - 1.0);
            }
            eig_res = std::sqrt(acc / static_cast<crd::f64>(un));
        }
    }

    // ---- Multi-RHS solve (nrhs=16): Cerid block-gemm vs Eigen (the amortized-L regime) ----
    const crd::usize nrhs = 16;
    crd::f64 t_cerid_mrhs = 0.0;
    crd::f64 t_eig_mrhs = 0.0;
    {
        crd::containers::Array<crd::f64> rb16(&g_alloc);
        rb16.resize(static_cast<crd::usize>(un) * nrhs);
        for (crd::usize c = 0; c < nrhs; ++c)
        {
            for (crd::u32 i = 0; i < un; ++i)
            {
                rb16[c * un + i] = b[i];
            }
        }
        crd::containers::Array<crd::f64> x16(&g_alloc);
        x16.resize(rb16.size());
        t_cerid_mrhs = best_ms(
            [&]()
            {
                for (crd::usize k = 0; k < rb16.size(); ++k)
                {
                    x16[k] = rb16[k];
                }
                (void)cf.solve({x16.data(), x16.size()}, nrhs);
            });
        if (!no_ref && llt.info() == Eigen::Success)
        {
            Eigen::MatrixXd eb16(n, static_cast<int>(nrhs));
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                for (crd::u32 i = 0; i < un; ++i)
                {
                    eb16(static_cast<int>(i), static_cast<int>(c)) = b[i];
                }
            }
            Eigen::MatrixXd ex16;
            t_eig_mrhs = best_ms([&]() { ex16 = llt.solve(eb16); });
        }
    }
    const crd::f64 mrhs_ratio = t_cerid_mrhs > 0 ? t_eig_mrhs / t_cerid_mrhs : 0.0;

    const crd::f64 fac_ratio = t_cerid_fac > 0 ? t_eig_fac / t_cerid_fac : 0.0;
    const crd::f64 slv_ratio = t_cerid_slv > 0 ? t_eig_slv / t_cerid_slv : 0.0;
    std::printf("  %-10s n=%-6d | FACTOR cerid=%8.2f ms eigen=%8.2f ms (%.2fx %s) | SOLVE cerid=%6.3f eigen=%6.3f "
                "(%.2fx) | fill cerid=%-9llu eigen=%-9lld | resid c=%.1e e=%.1e\n",
                name, n, t_cerid_fac, t_eig_fac, fac_ratio, (fac_ratio >= 1.0 ? "WIN" : "lose"), t_cerid_slv,
                t_eig_slv, slv_ratio, static_cast<unsigned long long>(cf.factor_nnz()), eig_lnz, cerid_res, eig_res);
    std::printf("             SOLVE x16: cerid=%8.3f ms eigen=%8.3f ms (%.2fx %s)\n", t_cerid_mrhs, t_eig_mrhs,
                mrhs_ratio, (mrhs_ratio >= 1.0 ? "WIN" : "lose"));
}
} // namespace

int main()
{
    // CRD_BENCH_THREADS=N caps the worker pool (also caps host load — the i9-14900K
    // all-core hazard). N=1 is the per-thread-efficiency profiling regime. 0/unset =
    // hardware_concurrency. Mirrors bench_hesap_cholesky_vs_cholmod.
    crd::jobs::Config cfg;
    if (const char* t = std::getenv("CRD_BENCH_THREADS"))
    {
        cfg.num_threads = static_cast<crd::u32>(std::strtoul(t, nullptr, 10));
    }
    crd::jobs::init(cfg);
    std::printf("[bench_hesap_cholesky] TREE-PARALLEL supernodal Cholesky factor+solve vs Eigen SimplicialLLT "
                "(same AMD-permuted matrix; %u workers). Targets: factor >=1.5x, solve >=1.2x.\n",
                crd::jobs::num_workers());
    run("bcsstk13");
    run("bcsstk24");
    run("bcsstk25");
    run("bcsstk30"); // not PD (verified) — bench reports + skips
    run("bcsstk32");
    run("bmwcra_1"); // GHS_psdef, n=148770 — 100k+ SPD (parallel factor scales)
    run("hood");     // GHS_psdef, n=220542
    run("ldoor");    // GHS_psdef, n=952203 — ~1M headline (fill ~144M)
    crd::jobs::shutdown();
    return 0;
}
