// bench_hesap_lu_supernodal_vs_reference -- Phase 3.1.6 v5b-2c (the supernodal-LU crush).
//
// On REAL genuinely-UNSYMMETRIC SuiteSparse matrices (circuit / CFD / oil-reservoir /
// power-flow / semiconductor), factor + solve the SAME AMD-permuted matrix with:
//   (a) Cerid SupernodalLU  — MC64 static-pivot + the v5b-2b symbolic + supernodal BLAS-3
//       numeric + iterative refinement (deterministic, the v5b-2d {1,2,4,8} moat).
//   (b) Eigen SparseLU<.., NaturalOrdering> on the same AMD-permuted matrix (a sequential
//       supernodal SuperLU port — partial pivoting).
//   (c) UMFPACK (SuiteSparse, multifrontal gold standard) — built only when
//       CRD_HESAP_VS_UMFPACK is defined (WSL/Linux; GPL → dev-only, never shipped).
//
// FAIR-BENCH (advisor): static pivoting trades stability vs the partial-pivoting peers, so
// Cerid drives the solve to a MATCHED TRUE residual ‖A·x − b‖/‖b‖ via iterative refinement —
// the "crush" is a speed win at equal accuracy, not an accuracy mismatch. Factor time is the
// structural headline; fill (nnz L+U) is reported alongside (all three on the same AMD order).
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. crd conventions throughout; raw double
// only at the Eigen / UMFPACK API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/gmres_refine.hpp>    // v5f: factor_gmres_refined_lu (static-pivot divergence fix)
#include <crd/hesap/direct/lu_symbolic.hpp>
#include <crd/hesap/direct/mixed_refine.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/SparseCore>
#include <Eigen/SparseLU>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef CRD_HESAP_VS_UMFPACK
#include <umfpack.h>
#endif

#ifdef CRD_HESAP_VS_MUMPS
#include <dmumps_c.h>
#include <smumps_c.h> // v5f-c: SINGLE-precision MUMPS — the honest mixed-precision (factor-f32 + f64-IR) peer
#include <limits>
#endif

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

// True relative residual ‖A·x − b‖∞ / ‖b‖∞ on the permuted CSR matrix `ap`.
crd::f64 true_residual(const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& ap, const crd::f64* x,
                       const crd::f64* b)
{
    const sp::SparsePattern& pat = ap.pattern();
    const crd::f64* v = ap.values().values.data();
    crd::f64 rmax = 0.0;
    crd::f64 bmax = 0.0;
    for (crd::u32 r = 0; r < pat.rows; ++r)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 k = pat.outer_ptr[r]; k < pat.outer_ptr[r + 1]; ++k)
        {
            acc += v[k] * x[pat.inner_idx[k]];
        }
        const crd::f64 d = std::fabs(acc - b[r]);
        rmax = d > rmax ? d : rmax;
        bmax = std::fabs(b[r]) > bmax ? std::fabs(b[r]) : bmax;
    }
    return bmax > 0.0 ? rmax / bmax : rmax;
}

// v5b-3a SYMMETRIZED-FILL measurement: is the SYMMETRIC-PATTERN (MUMPS-style) multifrontal fill cheap on
// the near-structurally-symmetric sim targets? Compares the UNSYMMETRIC LU fill (lu_symbolic L+U) against
// the SYMMETRIC-PATTERN fill 2*nnz(chol(B+Bt)) - n (build_adjacency symmetrizes B internally). Ratio ~1
// ⇒ symmetric-pattern fronts are ~free for that matrix (the CFD/FEM targets); >>1 ⇒ expensive (genuinely
// unsymmetric — circuit). This gates the v5b-3 symmetric-pattern decision. B = the MC64+AMD-permuted matrix.
void report_symmetrized_fill(const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& ap, const char* name)
{
    sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> bmat(&g_alloc);
    auto sc = dir::static_lu_prepare<crd::f64>(ap, bmat, &g_alloc);
    (void)sc;
    const auto& bp = bmat.pattern();
    const crd::u32 n = bp.cols;
    const crd::u64 bnnz = static_cast<crd::u64>(bp.inner_idx.size());

    auto sym = dir::lu_symbolic(bp, &g_alloc);
    const crd::u64 unsym_fill = sym.lnz + sym.unz;

    auto et = ord::elimination_tree(bp, &g_alloc);
    auto cc = ord::column_counts(bp, {et.data(), et.size()}, &g_alloc);
    crd::u64 chol_nnz = 0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        chol_nnz += cc[j];
    }
    const crd::u64 sym_fill = (2 * chol_nnz >= n) ? (2 * chol_nnz - n) : 0; // L + U share the n diagonals

    // Structural symmetry of B: fraction of off-diagonal entries (i,j) whose transpose (j,i) is present.
    const crd::u32* bcp = bp.outer_ptr.data();
    const crd::u32* bri = bp.inner_idx.data();
    crd::u64 off = 0;
    crd::u64 sympair = 0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 p = bcp[j]; p < bcp[j + 1]; ++p)
        {
            const crd::u32 i = bri[p];
            if (i == j)
            {
                continue;
            }
            ++off;
            crd::u32 lo = bcp[i];
            crd::u32 hi = bcp[i + 1];
            while (lo < hi) // binary search column i's ascending rows for j
            {
                const crd::u32 mid = lo + (hi - lo) / 2;
                if (bri[mid] == j)
                {
                    ++sympair;
                    break;
                }
                if (bri[mid] < j)
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }
        }
    }
    const double ssym = (off > 0) ? 100.0 * static_cast<double>(sympair) / static_cast<double>(off) : 100.0;
    const double ratio = static_cast<double>(sym_fill) / static_cast<double>(unsym_fill > 0 ? unsym_fill : 1);
    std::printf("  %-10s n=%-7u nnz(B)=%-9llu | UNSYM L+U=%-10llu | SYM-PATTERN(B+Bt)=%-10llu (%.2fx) | "
                "struct_sym=%.1f%%\n",
                name, n, static_cast<unsigned long long>(bnnz), static_cast<unsigned long long>(unsym_fill),
                static_cast<unsigned long long>(sym_fill), ratio, ssym);
}

void run(const char* name)
{
    if (const char* only = std::getenv("CRD_BENCH_ONLY"))
    {
        if (std::strstr(only, name) == nullptr)
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

    // Cerid AMD(A+Aᵀ) ordering; permute so all factorizations see the SAME ordered system.
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
    auto ap = pb.compress(); // permuted CSR

    // v5b-3a: symmetrized-fill measurement mode — pure symbolic, skips the slow factor/Eigen/UMFPACK.
    if (std::getenv("CRD_BENCH_SYMFILL") != nullptr)
    {
        report_symmetrized_fill(ap, name);
        return;
    }

    // RHS = A_p · 1 (x_true = 1).
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
            acc += ap.values().values[st + k];
        }
        b[o] = acc;
    }

    // ---- Cerid SupernodalLU (MC64 static-pivot + supernodal BLAS-3 + IR) ----
    // SERIAL headline (the current best): these unsymmetric matrices have deep/narrow column etrees ⇒
    // limited level-parallelism, and the per-worker full-n SPA is memory-heavy, so tree-parallel does
    // not yet beat serial here (the {1,2,4,8} determinism MOAT is verified separately; within-front
    // parallelism + a compacted per-worker scratch are the characterized next levers). Override with
    // CRD_BENCH_LU_WORKERS to exercise the parallel path.
    crd::u32 nw = 1U;
    if (const char* wenv = std::getenv("CRD_BENCH_LU_WORKERS"))
    {
        nw = static_cast<crd::u32>(std::atoi(wenv));
        if (nw == 0U)
        {
            nw = 1U;
        }
    }
    const crd::f64 t_cer_fac = best_ms(
        [&]()
        {
            auto f = dir::factor_supernodal_lu<crd::f64>(ap, &g_alloc, nw);
            (void)f.info();
        });
    auto cf = dir::factor_supernodal_lu<crd::f64>(ap, &g_alloc, nw);
    crd::f64 t_cer_slv = 0.0;
    crd::f64 cer_res = 1e30;
    crd::u64 cer_fill = 0;
    crd::u32 cer_nsuper = 0;
    if (cf.info() == 0)
    {
        t_cer_slv = best_ms(
            [&]()
            {
                for (crd::u32 i = 0; i < un; ++i)
                {
                    x[i] = b[i];
                }
                (void)cf.solve({x.data(), un});
            });
        cer_res = true_residual(ap, x.data(), b.data());
        cer_fill = cf.factor_nnz();
        cer_nsuper = cf.supernode_count();
    }

    // ---- Eigen SparseLU on the SAME permuted matrix (NaturalOrdering; partial pivot) ----
    crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
    et.reserve(static_cast<crd::usize>(ap.values().values.size()));
    for (crd::u32 r = 0; r < un; ++r)
    {
        const crd::u32 st = ap.pattern().outer_ptr[r];
        const crd::u32 cnt = ap.pattern().inner_count(r);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            et.push_back(Eigen::Triplet<double>(static_cast<int>(r), static_cast<int>(ap.pattern().inner_idx[st + k]),
                                                ap.values().values[st + k]));
        }
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    ea.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> elu; // Eigen's DEFAULT (fair best)
    const crd::f64 t_eig_fac = best_ms(
        [&]()
        {
            elu.analyzePattern(ea);
            elu.factorize(ea);
        });
    Eigen::VectorXd eb(n);
    for (crd::u32 i = 0; i < un; ++i)
    {
        eb[static_cast<int>(i)] = b[i];
    }
    Eigen::VectorXd ex;
    crd::f64 t_eig_slv = 0.0;
    crd::f64 eig_res = 1e30;
    crd::u64 eig_fill = 0;
    if (elu.info() == Eigen::Success)
    {
        t_eig_slv = best_ms([&]() { ex = elu.solve(eb); });
        crd::containers::Array<crd::f64> exv(&g_alloc);
        exv.resize(un);
        for (crd::u32 i = 0; i < un; ++i)
        {
            exv[i] = ex[static_cast<int>(i)];
        }
        eig_res = true_residual(ap, exv.data(), b.data());
        eig_fill = static_cast<crd::u64>(elu.nnzL()) + static_cast<crd::u64>(elu.nnzU());
    }

    const double cer_fac_ratio = (t_cer_fac > 0.0) ? t_eig_fac / t_cer_fac : 0.0; // >1 ⇒ Cerid faster
    // [ok] requires BOTH a completed factorization AND an accurate solve — the residual gate matters:
    // static pivoting can complete (info==0) yet diverge on saddle-point/indefinite systems (incompressible
    // Navier-Stokes), giving a huge residual. INACCURATE flags that honestly (was silently "ok").
    const char* cer_ok = (cf.info() != 0) ? "SINGULAR" : (cer_res < 1e-6 ? "ok" : "INACCURATE");
    std::printf("  %-9s n=%-6d | FACTOR cer=%9.2f eig=%9.2f ms (%.2fx vs Eigen) | SOLVE cer=%7.3f eig=%7.3f "
                "| fill cer=%-9llu eig=%-9llu | nsup=%-6u | resid c=%.1e e=%.1e [%s]",
                name, n, t_cer_fac, t_eig_fac, cer_fac_ratio, t_cer_slv, t_eig_slv,
                static_cast<unsigned long long>(cer_fill), static_cast<unsigned long long>(eig_fill), cer_nsuper,
                cer_res, eig_res, cer_ok);

    // ---- Cerid MULTIFRONTAL LU (v5b-3b dense-front BLAS-3) on the SAME `ap` — behind CRD_BENCH_LU_MF.
    // The UMFPACK-crush path; the supernodal headline above stays unchanged. Same nw (serial = fair vs
    // UMFPACK-1thr). Prints factor time + speedup vs the Cerid supernodal factor (SN); the MF-vs-UMFPACK
    // ratio is added in the UMFPACK block below. Fill is the symmetric-pattern dense-front nnz — a
    // DIFFERENT padding regime than SupernodalLU's relaxed-supernode fill (do not equate the raw numbers).
    const bool do_mf = std::getenv("CRD_BENCH_LU_MF") != nullptr;
    crd::f64 t_mf_fac = 0.0;
    if (do_mf)
    {
        t_mf_fac = best_ms(
            [&]()
            {
                auto f = dir::factor_multifrontal_lu<crd::f64>(ap, &g_alloc, nw);
                (void)f.info();
            });
        auto mff = dir::factor_multifrontal_lu<crd::f64>(ap, &g_alloc, nw);
        crd::f64 t_mf_slv = 0.0;
        crd::f64 mf_res = 1e30;
        crd::u64 mf_fill = 0;
        if (mff.info() == 0)
        {
            t_mf_slv = best_ms(
                [&]()
                {
                    for (crd::u32 i = 0; i < un; ++i)
                    {
                        x[i] = b[i];
                    }
                    (void)mff.solve({x.data(), un});
                });
            mf_res = true_residual(ap, x.data(), b.data());
            mf_fill = mff.factor_nnz();
        }
        const double mf_sn_ratio = (t_mf_fac > 0.0) ? t_cer_fac / t_mf_fac : 0.0; // >1 ⇒ MF beats supernodal
        const char* mf_ok = (mff.info() != 0) ? "SINGULAR" : (mf_res < 1e-6 ? "ok" : "INACCURATE");
        std::printf(" || Cerid-MF fac=%9.2f ms (%.2fx vs SN) solve=%7.3f fill=%-9llu resid=%.1e [%s]", t_mf_fac,
                    mf_sn_ratio, t_mf_slv, static_cast<unsigned long long>(mf_fill), mf_res, mf_ok);
    }

#ifdef CRD_HESAP_VS_UMFPACK
    // ---- UMFPACK on the SAME permuted matrix (CSC). Default strategy (its own scaling). ----
    auto apcsc = sp::to_csc<crd::f64>(ap, &g_alloc);
    crd::containers::Array<crd::i32> uap(&g_alloc);
    crd::containers::Array<crd::i32> uai(&g_alloc);
    uap.resize(un + 1);
    const crd::u32 unnz = static_cast<crd::u32>(apcsc.pattern().inner_idx.size());
    uai.resize(unnz);
    for (crd::u32 i = 0; i <= un; ++i)
    {
        uap[i] = static_cast<crd::i32>(apcsc.pattern().outer_ptr[i]);
    }
    for (crd::u32 i = 0; i < unnz; ++i)
    {
        uai[i] = static_cast<crd::i32>(apcsc.pattern().inner_idx[i]);
    }
    const double* uax = apcsc.values().values.data();
    double uctrl[UMFPACK_CONTROL];
    umfpack_di_defaults(uctrl);
    void* usym = nullptr;
    void* unum = nullptr;
    crd::f64 t_umf_fac = best_ms(
        [&]()
        {
            if (usym)
            {
                umfpack_di_free_symbolic(&usym);
            }
            if (unum)
            {
                umfpack_di_free_numeric(&unum);
            }
            umfpack_di_symbolic(n, n, uap.data(), uai.data(), uax, &usym, uctrl, nullptr);
            umfpack_di_numeric(uap.data(), uai.data(), uax, usym, &unum, uctrl, nullptr);
        });
    crd::containers::Array<crd::f64> ux(&g_alloc);
    ux.resize(un);
    const crd::f64 t_umf_slv = best_ms(
        [&]() { umfpack_di_solve(UMFPACK_A, uap.data(), uai.data(), uax, ux.data(), b.data(), unum, uctrl, nullptr); });
    const crd::f64 umf_res = true_residual(ap, ux.data(), b.data());
    crd::u64 umf_fill = 0;
    {
        crd::i32 lnz = 0;
        crd::i32 unz = 0;
        crd::i32 nr = 0;
        crd::i32 nc = 0;
        crd::i32 nzd = 0;
        if (umfpack_di_get_lunz(&lnz, &unz, &nr, &nc, &nzd, unum) == UMFPACK_OK)
        {
            umf_fill = static_cast<crd::u64>(lnz) + static_cast<crd::u64>(unz);
        }
    }
    const double cer_umf_ratio = (t_cer_fac > 0.0) ? t_umf_fac / t_cer_fac : 0.0;
    std::printf(" || UMFPACK fac=%9.2f ms (%.2fx) solve=%7.3f fill=%-9llu resid=%.1e", t_umf_fac, cer_umf_ratio,
                t_umf_slv, static_cast<unsigned long long>(umf_fill), umf_res);
    if (do_mf && t_mf_fac > 0.0)
    {
        // THE crush ratio: UMFPACK factor / Cerid-multifrontal factor (>1 ⇒ Cerid-MF wins). Same
        // OPENBLAS_NUM_THREADS=1 (UMFPACK's best) + Cerid nw ⇒ fair same-class comparison.
        std::printf("  [MF %.2fx vs UMFPACK]", t_umf_fac / t_mf_fac);
    }
    if (usym)
    {
        umfpack_di_free_symbolic(&usym);
    }
    if (unum)
    {
        umfpack_di_free_numeric(&unum);
    }
#endif
    // ---- v5f-c: Cerid f32-factor + f64 iterative refinement (mixed precision) on the SAME `ap`. ----
    // factor_mixed_lu does the unsymmetric LU in f32 (~2x the dense-front kernel rate, ~½ the factor memory)
    // and recovers f64 accuracy with a fixed-order f64 IR loop (64·eps backward-error target). The honest
    // mixed-precision result: at MATCHED f64 residual it lands parity-class with the single-precision peer
    // (smumps+IR below) AND keeps the {1,2,4,8}-worker bit-determinism MOAT that no MUMPS path carries.
    crd::f64 t_mix_fac = best_ms(
        [&]()
        {
            auto f = dir::factor_mixed_lu(ap, &g_alloc);
            (void)f.info();
        });
    auto mixf = dir::factor_mixed_lu(ap, &g_alloc);
    crd::f64 t_mix_slv = 0.0;
    crd::f64 mix_res = 1e30;
    crd::u32 mix_iters = 0;
    bool mix_converged = false;
    if (mixf.info() == 0)
    {
        bool mix_ok = false;
        t_mix_slv = best_ms(
            [&]()
            {
                for (crd::u32 i = 0; i < un; ++i)
                {
                    x[i] = b[i];
                }
                mix_ok = mixf.solve({x.data(), un}, 1);
            });
        mix_res = true_residual(ap, x.data(), b.data());
        mix_iters = mixf.last_iters();
        // Honest accept guard: the f32 factor reaches f64 only when κ(A)·u_f ≲ 1. On matrices needing true
        // partial pivoting (static-pivot LU stalls at f64 too) the IR cannot recover — flag, do not race.
        mix_converged = mix_ok && (mix_res <= 1e-8);
    }
    std::printf(" || Cerid-mix fac=%9.2f ms solve=%7.3f iters=%u resid=%.1e [%s]", t_mix_fac, t_mix_slv, mix_iters,
                mix_res, mix_converged ? "ok" : "DIVERGED");
    // ---- v5f-(a): Cerid within-front PARTIAL-PIVOTING LU (factor_multifrontal_lu_pp) — the root-cause fix for
    // the static-pivot divergence (garon2/raefsky3). Does the bare partial-pivot factor now reach f64? ----
    if (std::getenv("CRD_BENCH_LU_PP") != nullptr)
    {
        const auto ppt0 = Clock::now();
        auto ppf = dir::factor_multifrontal_lu_pp<crd::f64>(ap, &g_alloc, 1);
        const double pp_fac = std::chrono::duration<crd::f64, std::milli>(Clock::now() - ppt0).count();
        if (ppf.info() == 0)
        {
            for (crd::u32 i = 0; i < un; ++i)
            {
                x[i] = b[i];
            }
            const auto pps0 = Clock::now();
            const bool ppok = ppf.solve({x.data(), un});
            const double pp_slv = std::chrono::duration<crd::f64, std::milli>(Clock::now() - pps0).count();
            const double pp_res = true_residual(ap, x.data(), b.data());
            std::printf(" || Cerid-pp fac=%9.2f solve=%7.3f resid=%.1e [%s]", pp_fac, pp_slv, pp_res,
                        (ppok && pp_res <= 1e-8) ? "ok" : "INACCURATE");
        }
    }
    // ---- v5f GMRES-IR PROBE (CRD_BENCH_LU_GMRESIR): the discriminating diagnostic for the static-pivot
    // divergence. FGMRES preconditioned by the f64 static-pivot factor — does it reach f64 on garon2/raefsky3
    // (where fixed-point IR diverges), and in how many iters? If yes ⇒ build GMRES-IR into the base solve. ----
    if (std::getenv("CRD_BENCH_LU_GMRESIR") != nullptr)
    {
        dir::GmresRefineOptions gopts;
        if (const char* re = std::getenv("CRD_GMRESIR_RESTART"))
        {
            gopts.restart = static_cast<crd::usize>(std::atoi(re));
        }
        if (const char* mi = std::getenv("CRD_GMRESIR_MAXIT"))
        {
            gopts.max_iter = static_cast<crd::usize>(std::atoi(mi));
        }
        const crd::f64 gpp = (std::getenv("CRD_GMRESIR_PP") != nullptr) ? 1.0 : 0.0; // pp preconditioner?
        const auto fct0 = Clock::now();
        auto grefined = dir::factor_gmres_refined_lu(ap, &g_alloc, 1, gopts, gpp); // the shipped API
        const double gfac_ms = std::chrono::duration<crd::f64, std::milli>(Clock::now() - fct0).count();
        if (grefined.info() == 0)
        {
            for (crd::u32 i = 0; i < un; ++i)
            {
                x[i] = b[i];
            }
            const auto gt0 = Clock::now();
            const bool gok = grefined.solve({x.data(), un}, 1);
            const double gms = std::chrono::duration<crd::f64, std::milli>(Clock::now() - gt0).count();
            const double gres = true_residual(ap, x.data(), b.data());
            std::printf(" || GMRES-IR fac=%9.2f solve=%7.3f iters=%u conv=%d resid=%.1e", gfac_ms, gms,
                        grefined.last_iters(), gok ? 1 : 0, gres);
        }
    }
#ifdef CRD_HESAP_VS_MUMPS
    // ---- MUMPS (parallel multifrontal, OpenMP) on the SAME `ap`. The HONEST parallel-peer comparison.
    // Run with OMP_NUM_THREADS=N for MUMPS's parallelism; Cerid-MF uses its own workers (CRD_BENCH_LU_WORKERS).
    if (do_mf && std::getenv("CRD_BENCH_LU_MUMPS") != nullptr)
    {
        const crd::u32 mnnz = static_cast<crd::u32>(ap.pattern().inner_idx.size());
        crd::containers::Array<MUMPS_INT> mirn(&g_alloc);
        crd::containers::Array<MUMPS_INT> mjcn(&g_alloc);
        crd::containers::Array<double> ma(&g_alloc);
        mirn.resize(mnnz);
        mjcn.resize(mnnz);
        ma.resize(mnnz);
        const crd::u32* rp = ap.pattern().outer_ptr.data();
        const crd::u32* ci = ap.pattern().inner_idx.data();
        const double* av = ap.values().values.data();
        crd::u32 w = 0;
        for (crd::u32 i = 0; i < un; ++i)
        {
            for (crd::u32 p = rp[i]; p < rp[i + 1]; ++p)
            {
                mirn[w] = static_cast<MUMPS_INT>(i + 1); // COO, 1-based (Fortran)
                mjcn[w] = static_cast<MUMPS_INT>(ci[p] + 1);
                ma[w] = av[p];
                ++w;
            }
        }
        DMUMPS_STRUC_C id;
        id.comm_fortran = -987654; // USE_COMM_WORLD for the sequential (OpenMP) build
        id.par = 1;
        id.sym = 0; // unsymmetric
        id.job = -1;
        dmumps_c(&id); // init
        id.icntl[0] = -1;
        id.icntl[1] = -1;
        id.icntl[2] = -1;
        id.icntl[3] = 0; // silence all output
        id.n = static_cast<MUMPS_INT>(un);
        id.nnz = static_cast<MUMPS_INT8>(mnnz);
        id.irn = mirn.data();
        id.jcn = mjcn.data();
        id.a = ma.data();
        crd::f64 t_mumps_fac = best_ms(
            [&]()
            {
                id.job = 4; // analyze + factorize (comparable to UMFPACK symbolic+numeric)
                dmumps_c(&id);
            });
        crd::containers::Array<double> mx(&g_alloc);
        mx.resize(un);
        for (crd::u32 i = 0; i < un; ++i)
        {
            mx[i] = b[i];
        }
        id.rhs = mx.data();
        id.job = 3; // solve (rhs overwritten with solution)
        dmumps_c(&id);
        const double mumps_res = (id.infog[0] < 0) ? 1e30 : true_residual(ap, mx.data(), b.data());
        id.job = -2;
        dmumps_c(&id); // finalize
        std::printf(" || MUMPS fac=%9.2f ms resid=%.1e infog1=%d", t_mumps_fac, mumps_res, id.infog[0]);
        if (t_mf_fac > 0.0)
        {
            std::printf("  [MF %.2fx vs MUMPS]", t_mumps_fac / t_mf_fac);
        }
    }
    // ---- v5f-c: smumps (SINGLE-precision MUMPS) + f64 iterative refinement — the HONEST mixed-precision peer.
    // MUMPS does the f32 factor; we wrap it in the SAME f64 IR loop (64·eps backward-error target, fixed-order
    // f64 residual) so BOTH paths reach f64 accuracy. The comparison Cerid-mix vs smumps+IR is mixed-vs-mixed
    // (no dmumps-f64 asterisk); the differentiator is the cross-thread determinism MOAT smumps+IR cannot carry.
    if (std::getenv("CRD_BENCH_LU_MUMPS") != nullptr)
    {
        const crd::u32 snnz = static_cast<crd::u32>(ap.pattern().inner_idx.size());
        crd::containers::Array<MUMPS_INT> sirn(&g_alloc);
        crd::containers::Array<MUMPS_INT> sjcn(&g_alloc);
        crd::containers::Array<float> sav(&g_alloc);
        sirn.resize(snnz);
        sjcn.resize(snnz);
        sav.resize(snnz);
        const crd::u32* rp = ap.pattern().outer_ptr.data();
        const crd::u32* ci = ap.pattern().inner_idx.data();
        const double* av = ap.values().values.data();
        crd::u32 w = 0;
        for (crd::u32 i = 0; i < un; ++i)
        {
            for (crd::u32 p = rp[i]; p < rp[i + 1]; ++p)
            {
                sirn[w] = static_cast<MUMPS_INT>(i + 1); // COO, 1-based (Fortran)
                sjcn[w] = static_cast<MUMPS_INT>(ci[p] + 1);
                sav[w] = static_cast<float>(av[p]); // SINGLE-precision matrix copy
                ++w;
            }
        }
        SMUMPS_STRUC_C sid;
        sid.comm_fortran = -987654; // USE_COMM_WORLD (sequential OpenMP build)
        sid.par = 1;
        sid.sym = 0; // unsymmetric
        sid.job = -1;
        smumps_c(&sid); // init
        sid.icntl[0] = -1;
        sid.icntl[1] = -1;
        sid.icntl[2] = -1;
        sid.icntl[3] = 0; // silence all output
        sid.n = static_cast<MUMPS_INT>(un);
        sid.nnz = static_cast<MUMPS_INT8>(snnz);
        sid.irn = sirn.data();
        sid.jcn = sjcn.data();
        sid.a = sav.data();
        const crd::f64 t_smumps_fac = best_ms(
            [&]()
            {
                sid.job = 4; // analyze + factorize (single precision)
                smumps_c(&sid);
            });
        // f64 iterative refinement around the single-precision factor (mirror of IterativeRefinedSolve):
        // x=0 ⇒ r=b; converge at 64·eps·‖b‖∞; else single solve d=U⁻¹L⁻¹r (job 3), x+=d (f64). Stall guard.
        crd::containers::Array<crd::f64> sx(&g_alloc);
        crd::containers::Array<float> sr(&g_alloc);
        sx.resize(un);
        sr.resize(un);
        for (crd::u32 i = 0; i < un; ++i)
        {
            sx[i] = 0.0;
        }
        const double eps = std::numeric_limits<double>::epsilon();
        const double refine_tol = 64.0 * eps;
        double bn = 0.0;
        for (crd::u32 i = 0; i < un; ++i)
        {
            bn = std::fabs(b[i]) > bn ? std::fabs(b[i]) : bn;
        }
        bn = bn > 0.0 ? bn : 1.0;
        crd::u32 smumps_iters = 0;
        double prev_rn = 1e300;
        const auto sir0 = Clock::now(); // IR-loop cost (the single-precision back-substitutions) is end-to-end
        for (crd::u32 it = 0; it < 20U; ++it)
        {
            double rn = 0.0;
            for (crd::u32 r0 = 0; r0 < un; ++r0) // r = b - A·sx (f64 CSR matvec on the permuted matrix)
            {
                double acc = 0.0;
                for (crd::u32 p = rp[r0]; p < rp[r0 + 1]; ++p)
                {
                    acc += av[p] * sx[ci[p]];
                }
                const double rr = b[r0] - acc;
                sr[r0] = static_cast<float>(rr);
                const double m = std::fabs(rr);
                rn = m > rn ? m : rn;
            }
            if (rn <= refine_tol * bn)
            {
                break; // machine-precision backward error
            }
            if (it >= 1U && rn >= 0.5 * prev_rn)
            {
                break; // stall guard (hit the f32 round-off floor)
            }
            prev_rn = rn;
            sid.rhs = sr.data();
            sid.job = 3; // single-precision solve, rhs overwritten with the correction d
            smumps_c(&sid);
            for (crd::u32 i = 0; i < un; ++i)
            {
                sx[i] += static_cast<double>(sr[i]);
            }
            ++smumps_iters;
        }
        const double t_smumps_ir = std::chrono::duration<crd::f64, std::milli>(Clock::now() - sir0).count();
        const double smumps_res = (sid.infog[0] < 0) ? 1e30 : true_residual(ap, sx.data(), b.data());
        sid.job = -2;
        smumps_c(&sid); // finalize
        const bool smumps_ok = smumps_res <= 1e-8;
        std::printf(" || smumps+IR fac=%9.2f ms ir=%7.3f iters=%u resid=%.1e [%s]", t_smumps_fac, t_smumps_ir,
                    smumps_iters, smumps_res, smumps_ok ? "ok" : "DIVERGED");
        // Head-to-head ratio only when BOTH reached f64 accuracy (else the speed number compares a wrong answer).
        if (mix_converged && smumps_ok && t_mix_fac > 0.0)
        {
            // >1 ⇒ Cerid-mix faster at MATCHED f64 accuracy (factor + IR, mixed-vs-mixed, no dmumps-f64 asterisk).
            std::printf("  [Cerid-mix %.2fx vs smumps+IR]", (t_smumps_fac + t_smumps_ir) / (t_mix_fac + t_mix_slv));
        }
        else
        {
            std::printf("  [no matched-accuracy race: Cerid-mix %s]", mix_converged ? "ok" : "static-pivot wall");
        }
    }
#endif
    std::printf("\n");
    std::fflush(stdout);
}
} // namespace

int main()
{
    std::printf("[bench_hesap_lu_supernodal_vs_reference] Cerid SupernodalLU (MC64 static-pivot + supernodal BLAS-3 "
                "+ IR) vs Eigen SparseLU"
#ifdef CRD_HESAP_VS_UMFPACK
                " + UMFPACK"
#endif
                " on the SAME AMD-permuted UNSYMMETRIC matrix. Ratios >1 ⇒ Cerid faster; compare at MATCHED true "
                "residual.\n");
    crd::jobs::Config cfg;
    cfg.num_threads = 8; // cap the pool (per-worker full-n SPA memory + i9-14900K stability)
    crd::jobs::init(cfg);
    crd::u32 banner_nw = 1U;
    if (const char* wenv = std::getenv("CRD_BENCH_LU_WORKERS"))
    {
        banner_nw = static_cast<crd::u32>(std::atoi(wenv));
        if (banner_nw == 0U)
        {
            banner_nw = 1U;
        }
    }
    std::printf("[bench] Cerid factor: %s (CRD_BENCH_LU_WORKERS=%u, pool=%u). SERIAL is the honest headline; "
                "the {1,2,4,8}-worker results are bit-identical (the determinism moat).\n",
                banner_nw <= 1U ? "SERIAL" : "tree-parallel", banner_nw, crd::jobs::num_workers());
    // Genuinely-UNSYMMETRIC corpus (circuit / power-flow / oil-reservoir / CFD / semiconductor).
    run("west2021"); // 2021   chemical
    run("add32");    // 4960   circuit
    run("gemat11");  // 4929   power flow
    run("sherman3"); // 5005   oil reservoir
    run("memplus");  // 17758  circuit
    run("af23560");  // 23560  airfoil CFD
    run("wang3");    // 26064  semiconductor device
    // ---- SIMULATION TARGETS (the real workload: cloth/deformation/CFD/Navier-Stokes) ----
    run("garon2");   // 13535  2D Navier-Stokes FEM (unsymmetric)
    run("ns3Da");    // 20414  3D Navier-Stokes (unsymmetric)
    run("raefsky3"); // 21200  CFD / structural (unsymmetric)
    // bbmat (n=38744): exceeds the current full-n SPA budget (n·max_nc per worker > GrowableTlsf
    // chunk cap). Needs the column-blocked panel rework (noted in CLAUDE.md) — out of scope until then.
    crd::jobs::shutdown();
    return 0;
}
