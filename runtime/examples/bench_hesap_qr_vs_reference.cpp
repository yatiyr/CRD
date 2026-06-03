// bench_hesap_qr_vs_reference -- Phase 3.1.6 v5c-1d (multifrontal QR scoreboard).
//
// On real SuiteSparse matrices: factor + solve the SAME AMD(AᵀA)-column-ordered
// matrix with (a) Cerid MultifrontalQR and (b) Eigen SparseQR<.., NaturalOrdering>
// (fed the already column-permuted matrix, so both see the identical ordering and
// the comparison isolates the factorization engine). QR fill is governed by
// chol(AᵀA), so the fill-reducing COLUMN order is AMD(AᵀA) (the COLAMD-equivalent).
//
// HONEST framing (v5c-1 is correctness-first, unblocked serial Householder + an
// explicitly-formed AᵀA symbolic): EXPECTED to lose to Eigen SparseQR until the
// v5c-1d perf levers (blocked-WY + staircase + relaxed-front amalgamation +
// tree-parallel + the implicit AᵀA-free symbolic) land. The gate here is the
// residual (correctness) + a measured baseline to crush from. SuiteSparseQR/SPQR
// (the gold standard) is the WSL bench (CRD_BUILD_HESAP_VS_SUITESPARSE).
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. crd conventions throughout;
// raw double only at the Eigen API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/multifrontal_qr.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/SparseCore>
#include <Eigen/SparseQR>
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

bool read_mtx(const char* path, crd::i32& mrows, crd::i32& ncols, crd::containers::Array<Trip>& trips)
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
            mrows = rows;
            ncols = cols;
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
    crd::i32 mrows = 0;
    crd::i32 ncols = 0;
    crd::containers::Array<Trip> trips(&g_alloc);
    if (!read_mtx(path.c_str(), mrows, ncols, trips))
    {
        std::printf("  %-12s SKIP (not found)\n", name);
        return;
    }
    const crd::u32 um = static_cast<crd::u32>(mrows);
    const crd::u32 un = static_cast<crd::u32>(ncols);

    // CSC of A (m×n), then the QR fill-reducing COLUMN order = AMD(AᵀA) (COLAMD-equivalent).
    sp::TripletBuilder<crd::f64> tb(&g_alloc, um, un);
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();                         // CSR
    auto acsc0 = sp::to_csc<crd::f64>(a, &g_alloc); // CSC (unordered)
    auto ata = dir::ata_pattern(acsc0.pattern(), &g_alloc);
    auto perm = ord::amd_order(ata, &g_alloc); // column ordering on AᵀA (size n)

    // Permute COLUMNS of A by perm (A · P_c); rows untouched. Both engines factor A_p (m×n).
    sp::TripletBuilder<crd::f64> pb(&g_alloc, um, un);
    pb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        pb.add(static_cast<crd::u32>(t.r), perm.inv_perm[static_cast<crd::u32>(t.c)], t.v);
    }
    auto ap = pb.compress();                        // permuted CSR (m×n)
    auto acsc = sp::to_csc<crd::f64>(ap, &g_alloc); // CSC for the column-oriented QR

    // ---- Cerid MultifrontalQR ----
    const crd::f64 t_cerid_fac = best_ms(
        [&]()
        {
            auto f = dir::factor_multifrontal_qr<crd::f64>(
                acsc.pattern(), {acsc.values().values.data(), acsc.values().values.size()}, &g_alloc);
            (void)f.info();
        });
    auto cf = dir::factor_multifrontal_qr<crd::f64>(
        acsc.pattern(), {acsc.values().values.data(), acsc.values().values.size()}, &g_alloc);

    // RHS b = A_p · 1 (length m; x_true = 1 over the n columns ⇒ consistent LS recovers x = 1).
    crd::containers::Array<crd::f64> b(&g_alloc);
    crd::containers::Array<crd::f64> x(&g_alloc);
    b.resize(um);
    x.resize(un);
    for (crd::u32 o = 0; o < um; ++o)
    {
        crd::f64 acc = 0.0;
        const crd::u32 st = ap.pattern().outer_ptr[o];
        const crd::u32 cnt = ap.pattern().inner_count(o);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            acc += ap.values().values[st + k]; // row o · 1
        }
        b[o] = acc;
    }
    crd::f64 t_cerid_slv = 0.0;
    crd::f64 cerid_res = 0.0;
    crd::u64 cerid_fill = cf.r_nnz();
    if (cf.info() == 0)
    {
        t_cerid_slv =
            best_ms([&]() { (void)cf.least_squares({b.data(), um}, {x.data(), un}, 1); });
        for (crd::u32 i = 0; i < un; ++i)
        {
            cerid_res += (x[i] - 1.0) * (x[i] - 1.0);
        }
        cerid_res = std::sqrt(cerid_res / static_cast<crd::f64>(un));
    }

    // ---- Eigen SparseQR on the SAME permuted m×n matrix, NaturalOrdering ----
    crd::containers::Array<Eigen::Triplet<double>> et(&g_alloc);
    et.reserve(static_cast<crd::usize>(ap.values().values.size()));
    for (crd::u32 r = 0; r < um; ++r)
    {
        const crd::u32 st = ap.pattern().outer_ptr[r];
        const crd::u32 cnt = ap.pattern().inner_count(r);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            et.push_back(Eigen::Triplet<double>(static_cast<int>(r), static_cast<int>(ap.pattern().inner_idx[st + k]),
                                                ap.values().values[st + k]));
        }
    }
    Eigen::SparseMatrix<double> ea(mrows, ncols);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    ea.makeCompressed();
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::NaturalOrdering<int>> eqr;
    const crd::f64 t_eig_fac = best_ms(
        [&]()
        {
            eqr.analyzePattern(ea);
            eqr.factorize(ea);
        });
    Eigen::VectorXd eb(mrows);
    for (crd::u32 i = 0; i < um; ++i)
    {
        eb[static_cast<int>(i)] = b[i];
    }
    Eigen::VectorXd ex;
    const crd::f64 t_eig_slv = best_ms([&]() { ex = eqr.solve(eb); });
    double eig_res = 0.0;
    if (eqr.info() == Eigen::Success)
    {
        for (crd::u32 i = 0; i < un; ++i)
        {
            eig_res += (ex[static_cast<int>(i)] - 1.0) * (ex[static_cast<int>(i)] - 1.0);
        }
        eig_res = std::sqrt(eig_res / static_cast<double>(un));
    }
    const crd::u64 eig_fill = eqr.info() == Eigen::Success ? static_cast<crd::u64>(eqr.matrixR().nonZeros()) : 0;

    const char* cerid_ok = cf.info() == 0 ? "ok" : "RANKDEF";
    const double fac_ratio = (t_cerid_fac > 0.0) ? t_eig_fac / t_cerid_fac : 0.0; // >1 ⇒ Cerid faster
    const double slv_ratio = (t_cerid_slv > 0.0) ? t_eig_slv / t_cerid_slv : 0.0;
    std::printf("  %-12s %5dx%-5d | FACTOR cerid=%9.2f eigen=%9.2f ms (%.2fx) | SOLVE cerid=%7.3f eigen=%7.3f (%.2fx) "
                "| Rnnz c=%-9llu e=%-9llu | resid c=%.1e e=%.1e [%s]\n",
                name, mrows, ncols, t_cerid_fac, t_eig_fac, fac_ratio, t_cerid_slv, t_eig_slv, slv_ratio,
                static_cast<unsigned long long>(cerid_fill), static_cast<unsigned long long>(eig_fill), cerid_res,
                eig_res, cerid_ok);
    std::fflush(stdout);
}
} // namespace

int main()
{
    std::printf("[bench_hesap_qr_vs_reference] Cerid MultifrontalQR vs Eigen SparseQR (NaturalOrdering on the same "
                "AMD(AᵀA)-column-ordered matrix). v5c-1 = correctness-first baseline; gate = residual.\n");
    // Small/medium SPD-structural corpus (square, full rank) — these complete fast even with the
    // v5c-1 UNBLOCKED numeric. Larger matrices (bcsstk25 15k+) need the v5c-1d BLOCKED-WY lever +
    // the AᵀA-free implicit symbolic before they are tractable: the unblocked Householder on the
    // big dense near-root fronts is O(n³) and stalls. Run them explicitly via CRD_BENCH_ONLY once
    // blocked-WY lands (e.g. CRD_BENCH_ONLY=bcsstk25). Default corpus stays fast/stall-free.
    // QR's REAL domain: RECTANGULAR least-squares (m > n). The classic Harwell-Boeing LS test set.
    std::printf("[rectangular least-squares — QR's home turf]\n");
    run("well1033");  // 1033 x 320, well-conditioned
    run("illc1033");  // 1033 x 320, ill-conditioned
    run("well1850");  // 1850 x 712, well-conditioned
    run("illc1850");  // 1850 x 712, ill-conditioned
    std::printf("[square structural — both engines pay the chol(AᵀA) fill]\n");
    run("bcsstk13");
    run("bcsstk24");
    // NOTE: bcsstk25 (15k square SPD) and larger structural matrices are the WRONG tool for QR —
    // nnz(chol(AᵀA)) ≫ nnz(chol(A)) (AᵀA squares the fill), so QR factor storage hits GBs and is
    // hugely expensive (you'd use Cholesky for these). QR's real target is RECTANGULAR least-squares
    // (m > n). Add a rectangular-LS corpus + the AᵀA-free implicit symbolic + a front-storage stack
    // before benchmarking large matrices here. (`CRD_BENCH_ONLY=bcsstk25` to force it for diagnostics.)
    return 0;
}
