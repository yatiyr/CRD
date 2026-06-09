// bench_hesap_cholesky_vs_cholmod -- Phase 3.1.6 v5a-2 rigorous oracle.
//
// Races Cerid's TREE-PARALLEL supernodal Cholesky against SuiteSparse CHOLMOD —
// THE gold-standard supernodal sparse Cholesky (Davis et al.; the engine behind
// MATLAB chol, Eigen's CholmodSupernodalLLT, scikit-sparse). Our other v5a bench
// only raced Eigen's SIMPLICIAL LLT, a weaker scalar peer; CHOLMOD is the real
// supernodal floor.
//
// FAIR FIGHT: both factor the SAME Cerid-AMD-permuted matrix. CHOLMOD is forced
// to CHOLMOD_SUPERNODAL with CHOLMOD_NATURAL ordering (no second reorder — we
// already permuted) so the comparison is pure numeric+scheduling speed on an
// identical elimination order. CHOLMOD's dense panels and Cerid's gemm both run
// on OpenBLAS (run scripts/setup-cholmod-ref.sh; match thread counts via
// OMP_NUM_THREADS / OPENBLAS_NUM_THREADS == jobs::num_workers()).
//
// Built only when CRD_BUILD_HESAP_VS_CHOLMOD=ON (Linux/WSL; GPL-encumbered
// supernodal module ⇒ dev-only, never shipped, never in CI release). crd
// conventions throughout; raw double only at the CHOLMOD C-API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <suitesparse/cholmod.h>

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
    // Tuning filter: CRD_BENCH_ONLY=<name> runs just that matrix (fast, low-load threshold sweeps).
    if (const char* only = std::getenv("CRD_BENCH_ONLY"))
    {
        if (std::strcmp(only, name) != 0)
        {
            return;
        }
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

    // Cerid AMD ordering; permute the matrix so Cerid + CHOLMOD see the SAME order.
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

    // ---- Cerid supernodal Cholesky (TREE-PARALLEL) ----
    const crd::u32 nw = crd::jobs::num_workers();
    const crd::f64 t_cerid_fac = best_ms(
        [&]()
        {
            auto f =
                dir::factor_supernodal_cholesky<crd::f64>(ap.pattern(), apvals, &g_alloc, dir::kSupernodeRelax, nw);
            (void)f.info();
        });
    auto cf = dir::factor_supernodal_cholesky<crd::f64>(ap.pattern(), apvals, &g_alloc, dir::kSupernodeRelax, nw);
    if (cf.info() != 0)
    {
        std::printf("  %-10s Cerid NOT-SPD (info=%zu) — skipping\n", name, cf.info());
        return;
    }
    // AMALGAMATION DIAGNOSTIC (v7-e-2): Cerid's own supernode count + nc histogram, to compare directly against
    // CHOLMOD's below. "Fewer fill yet slower factor" is the signature of CONSERVATIVE amalgamation — if Cerid
    // has many more / much thinner supernodes than CHOLMOD, the gap is supernode STRUCTURE (a tunable nrelax /
    // merge-rule difference), not a fixed per-flop kernel ceiling.
    {
        const auto& sym = cf.symbolic();
        crd::i64 hist[6] = {0, 0, 0, 0, 0, 0}; // nc: 1 | 2-4 | 5-8 | 9-16 | 17-32 | 33+
        crd::u32 maxnc = 0;
        // Theoretical-MINIMUM Cholesky flops on CERID's own supernodes (symmetry-aware): Σ_cols (m-i)² ≈ n³/3 for
        // dense — the n³/3-flop standard (Golub & Van Loan). Matches cc.fl convention (validated: CHOLMOD does cc.fl
        // in 3.19s single-core = 66 GF/s ≤ peak). The EXECUTED gemm flops (CHOLPROF below) vs this MIN = redundancy.
        double cerid_min_fl = 0.0;
        for (crd::u32 s = 0; s < sym.nsuper; ++s)
        {
            const crd::u32 nc = sym.scol[s + 1] - sym.scol[s];
            const crd::u32 m = sym.srowp[s + 1] - sym.srowp[s]; // total rows in the panel (incl. diagonal block)
            maxnc = nc > maxnc ? nc : maxnc;
            const int bn = nc == 1 ? 0 : nc <= 4 ? 1 : nc <= 8 ? 2 : nc <= 16 ? 3 : nc <= 32 ? 4 : 5;
            ++hist[bn];
            for (crd::u32 i = 0; i < nc; ++i)
            {
                const double r = static_cast<double>(m - i);
                cerid_min_fl += r * r;
            }
        }
        std::printf("  %-10s CERID Cholesky-min-flops=%.2fe9 (symmetry-aware minimum on Cerid's supernodes)\n", name,
                    cerid_min_fl * 1e-9);
        std::printf("  %-10s CERID   nsuper=%lld maxnc=%lld | nc[1|2-4|5-8|9-16|17-32|33+]=%lld %lld %lld %lld %lld "
                    "%lld\n",
                    name, static_cast<long long>(sym.nsuper), static_cast<long long>(maxnc),
                    static_cast<long long>(hist[0]), static_cast<long long>(hist[1]), static_cast<long long>(hist[2]),
                    static_cast<long long>(hist[3]), static_cast<long long>(hist[4]), static_cast<long long>(hist[5]));
    }
    crd::containers::Array<crd::f64> b(&g_alloc);
    crd::containers::Array<crd::f64> x(&g_alloc);
    b.resize(un);
    x.resize(un);
    for (crd::u32 o = 0; o < un; ++o)
    {
        crd::f64 acc = 0.0;
        const crd::u32 st = ap.pattern().outer_ptr[o];
        const crd::u32 cnt = ap.pattern().inner_count(o);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            acc += apvals[st + k]; // x_true = 1
        }
        b[o] = acc;
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

    const crd::usize nrhs = 16;
    crd::containers::Array<crd::f64> rb16(&g_alloc);
    crd::containers::Array<crd::f64> x16(&g_alloc);
    rb16.resize(static_cast<crd::usize>(un) * nrhs);
    x16.resize(rb16.size());
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < un; ++i)
        {
            rb16[c * un + i] = b[i];
        }
    }
    const crd::f64 t_cerid_mrhs = best_ms(
        [&]()
        {
            for (crd::usize k = 0; k < rb16.size(); ++k)
            {
                x16[k] = rb16[k];
            }
            (void)cf.solve({x16.data(), x16.size()}, nrhs);
        });

    // ---- CHOLMOD supernodal LL^T on the SAME permuted matrix, NATURAL ordering ----
    // Feed the full symmetric matrix as CSC (CSR-of-symmetric == CSC) with stype=-1
    // (CHOLMOD reads the lower triangle). Convert Cerid u32 index arrays to int.
    cholmod_common cc;
    cholmod_start(&cc);
    cc.supernodal = CHOLMOD_SUPERNODAL; // force the supernodal path (the fair peer)
    cc.final_ll = 1;                    // keep LL^T (not LDL^T)
    cc.nmethods = 1;
    cc.method[0].ordering = CHOLMOD_NATURAL; // we already AMD-permuted
    cc.postorder = 0;

    const crd::u32 innz = static_cast<crd::u32>(ap.values().values.size());
    crd::containers::Array<int> cp(&g_alloc);
    crd::containers::Array<int> ci(&g_alloc);
    cp.resize(static_cast<crd::usize>(un) + 1);
    ci.resize(innz);
    for (crd::u32 o = 0; o <= un; ++o)
    {
        cp[o] = static_cast<int>(ap.pattern().outer_ptr[o]);
    }
    for (crd::u32 k = 0; k < innz; ++k)
    {
        ci[k] = static_cast<int>(ap.pattern().inner_idx[k]);
    }
    cholmod_sparse A;
    std::memset(&A, 0, sizeof(A));
    A.nrow = un;
    A.ncol = un;
    A.nzmax = innz;
    A.p = cp.data();
    A.i = ci.data();
    A.x = const_cast<crd::f64*>(ap.values().values.data());
    A.stype = -1; // symmetric, lower triangle used
    A.itype = CHOLMOD_INT;
    A.xtype = CHOLMOD_REAL;
    A.dtype = CHOLMOD_DOUBLE;
    A.sorted = 1;
    A.packed = 1;

    // FAIR: Cerid's factor_supernodal_cholesky times symbolic+numeric, so CHOLMOD must
    // time analyze+factorize too (not just numeric factorize). Re-analyze each rep.
    cholmod_factor* L = nullptr;
    const crd::f64 t_chol_fac = best_ms(
        [&]()
        {
            if (L != nullptr)
            {
                cholmod_free_factor(&L, &cc);
            }
            L = cholmod_analyze(&A, &cc); // symbolic (natural order)
            cholmod_factorize(&A, L, &cc);
        });
    const bool chol_ok = (cc.status >= 0) && (L != nullptr) && (L->minor >= L->n);
    long long chol_lnz = (L != nullptr) ? static_cast<long long>(L->xsize) : -1;
    std::printf("  %-10s CHOLMOD flops cc.fl=%.2fe9 (CHOLMOD's necessary factor flops) | fill=%lld\n", name, cc.fl * 1e-9,
                chol_lnz);

    // v5a-4 AMALGAMATION PROBE: CHOLMOD's supernode count + size histogram. Cerid factors bmwcra
    // as 16007 supernodes (11813 of them nc 2-4 = tiny BLAS-1/2) with 87803 cmod calls. If CHOLMOD
    // is far fewer/fatter, aggressive amalgamation (its "more fill yet faster" identity) is a
    // first-order divergence — fewer cmod dispatches + fatter BLAS-3 + better 8T scaling.
    if (L != nullptr && L->is_super)
    {
        const auto nsuper = static_cast<crd::i64>(L->nsuper);
        const auto* super = static_cast<const crd::i32*>(L->super);
        crd::i64 hist[6] = {0, 0, 0, 0, 0, 0}; // nc: 1 | 2-4 | 5-8 | 9-16 | 17-32 | 33+
        crd::i64 maxnc = 0;
        for (crd::i64 s = 0; s < nsuper; ++s)
        {
            const crd::i64 nc = super[s + 1] - super[s];
            maxnc = nc > maxnc ? nc : maxnc;
            const int bn = nc == 1 ? 0 : nc <= 4 ? 1 : nc <= 8 ? 2 : nc <= 16 ? 3 : nc <= 32 ? 4 : 5;
            ++hist[bn];
        }
        std::printf("  %-10s CHOLMOD nsuper=%lld maxnc=%lld | nc[1|2-4|5-8|9-16|17-32|33+]=%lld %lld %lld %lld %lld "
                    "%lld\n",
                    name, static_cast<long long>(nsuper), static_cast<long long>(maxnc),
                    static_cast<long long>(hist[0]), static_cast<long long>(hist[1]), static_cast<long long>(hist[2]),
                    static_cast<long long>(hist[3]), static_cast<long long>(hist[4]), static_cast<long long>(hist[5]));
    }

    crd::f64 chol_res = -1.0;
    crd::f64 t_chol_slv = 0.0;
    crd::f64 t_chol_mrhs = 0.0;
    if (chol_ok)
    {
        cholmod_dense* bd = cholmod_allocate_dense(un, 1, un, CHOLMOD_REAL, &cc);
        auto* bx = static_cast<crd::f64*>(bd->x);
        for (crd::u32 i = 0; i < un; ++i)
        {
            bx[i] = b[i];
        }
        cholmod_dense* xd = nullptr;
        t_chol_slv = best_ms(
            [&]()
            {
                if (xd != nullptr)
                {
                    cholmod_free_dense(&xd, &cc);
                }
                xd = cholmod_solve(CHOLMOD_A, L, bd, &cc);
            });
        const auto* xx = static_cast<const crd::f64*>(xd->x);
        crd::f64 acc = 0.0;
        for (crd::u32 i = 0; i < un; ++i)
        {
            acc += (xx[i] - 1.0) * (xx[i] - 1.0);
        }
        chol_res = std::sqrt(acc / static_cast<crd::f64>(un));
        cholmod_free_dense(&xd, &cc);

        cholmod_dense* bd16 = cholmod_allocate_dense(un, nrhs, un, CHOLMOD_REAL, &cc);
        auto* bx16 = static_cast<crd::f64*>(bd16->x);
        for (crd::usize c = 0; c < nrhs; ++c)
        {
            for (crd::u32 i = 0; i < un; ++i)
            {
                bx16[c * un + i] = b[i]; // CHOLMOD dense is column-major, ld=un
            }
        }
        cholmod_dense* xd16 = nullptr;
        t_chol_mrhs = best_ms(
            [&]()
            {
                if (xd16 != nullptr)
                {
                    cholmod_free_dense(&xd16, &cc);
                }
                xd16 = cholmod_solve(CHOLMOD_A, L, bd16, &cc);
            });
        cholmod_free_dense(&xd16, &cc);
        cholmod_free_dense(&bd16, &cc);
        cholmod_free_dense(&bd, &cc);
    }

    const crd::f64 fac_ratio = t_cerid_fac > 0 ? t_chol_fac / t_cerid_fac : 0.0;
    const crd::f64 slv_ratio = t_cerid_slv > 0 ? t_chol_slv / t_cerid_slv : 0.0;
    const crd::f64 mrhs_ratio = t_cerid_mrhs > 0 ? t_chol_mrhs / t_cerid_mrhs : 0.0;
    std::printf("  %-10s n=%-7d | FACTOR cerid=%9.2f cholmod=%9.2f ms (%.2fx %s) | SOLVE cerid=%7.3f cholmod=%7.3f "
                "(%.2fx) | fill cerid=%-10llu cholmod=%-10lld | resid c=%.1e h=%.1e\n",
                name, n, t_cerid_fac, t_chol_fac, fac_ratio, (fac_ratio >= 1.0 ? "WIN" : "lose"), t_cerid_slv,
                t_chol_slv, slv_ratio, static_cast<unsigned long long>(cf.factor_nnz()), chol_lnz, cerid_res, chol_res);
    std::printf("             SOLVE x16: cerid=%9.3f cholmod=%9.3f ms (%.2fx %s)\n", t_cerid_mrhs, t_chol_mrhs,
                mrhs_ratio, (mrhs_ratio >= 1.0 ? "WIN" : "lose"));

    cholmod_free_factor(&L, &cc);
    cholmod_finish(&cc);
}
} // namespace

extern "C" void openblas_set_num_threads(int); // FORCE CHOLMOD's BLAS thread count at runtime (env unreliable)

int main()
{
    // Match Cerid's worker count to CHOLMOD's BLAS thread count for a fair fight:
    // set CRD_BENCH_THREADS=N and OMP_NUM_THREADS=OPENBLAS_NUM_THREADS=N to the same N
    // (also caps host load — the i9-14900K all-core hazard). 0/unset = hardware_concurrency.
    crd::jobs::Config cfg;
    crd::u32 bench_threads = 0;
    if (const char* t = std::getenv("CRD_BENCH_THREADS"))
    {
        cfg.num_threads = static_cast<crd::u32>(std::strtoul(t, nullptr, 10));
        bench_threads = cfg.num_threads;
    }
    // The env (OPENBLAS_NUM_THREADS) is NOT reliably honored — CHOLMOD silently multithreaded large fronts in the
    // "1T" runs (measured >peak GFLOP/s). Force it at runtime so the head-to-head is genuinely matched-thread.
    if (bench_threads > 0)
    {
        openblas_set_num_threads(static_cast<int>(bench_threads));
        std::printf("[bench] forced openblas_set_num_threads(%u)\n", bench_threads);
    }
    crd::jobs::init(cfg);
    std::printf("[bench_hesap_cholesky_vs_cholmod] Cerid TREE-PARALLEL supernodal vs SuiteSparse CHOLMOD "
                "(supernodal, natural order on the same AMD-permuted matrix), workers=%u\n",
                crd::jobs::num_workers());
    run("bcsstk13");
    run("bcsstk24");
    run("bcsstk25");
    run("bmwcra_1");
    run("hood");
    run("ldoor");
    // v7-e-2: NLS-derived JᵀJ from the 3D elastic-lattice (generated by dump_nls_lattice_jtj). CHOLMOD == the
    // factorization Ceres-sparse uses ⇒ a per-factor win here on the ACTUAL NLS normal matrix is the honest crush
    // evidence (SKIP if not generated). Sizes match dump_nls_lattice_jtj's defaults + the larger headline.
    run("nls_lat12");
    run("nls_lat16");
    run("nls_lat20");
    run("nls_lat24");
    run("nls_lat28");
    run("nls_lat32");
    crd::jobs::shutdown();
    return 0;
}
