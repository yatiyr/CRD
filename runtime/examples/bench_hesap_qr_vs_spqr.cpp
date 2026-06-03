// bench_hesap_qr_vs_spqr -- Phase 3.1.6 v5c-1d gold-standard oracle.
//
// Races Cerid's MultifrontalQR against SuiteSparse SPQR (SuiteSparseQR, Davis
// Algorithm 915) — THE gold-standard multifrontal rank-revealing sparse QR
// (the engine behind MATLAB's sparse qr / mldivide for least-squares). Our
// other QR bench only raced Eigen SparseQR (a weak left-looking scalar peer);
// SPQR is the real multifrontal floor.
//
// FAIR FIGHT: both factor the SAME AMD(AᵀA)-column-permuted m×n matrix. SPQR is
// forced to SPQR_ORDERING_NATURAL (no second reorder — we already permuted) and
// SPQR_NO_TOL (no rank detection, matching Cerid v5c-1's full-rank assumption),
// so the comparison is pure numeric+scheduling on an identical column order.
// SERIAL fight (Cerid v5c-1 QR is serial): SPQR_grain=1 + run with
// OPENBLAS_NUM_THREADS=1 so SPQR's BLAS is single-threaded too (no
// parallel-vs-serial asterisk).
//
// Built only when CRD_BUILD_HESAP_VS_SPQR=ON (Linux/WSL; SPQR is GPL ⇒ dev-only,
// never shipped, never in CI release). crd conventions throughout; raw double
// only at the SPQR/CHOLMOD C-API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/multifrontal_qr.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <suitesparse/SuiteSparseQR.hpp>

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

void run(const char* name, cholmod_common* cc)
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

    // CSC of A (m×n), then the QR fill-reducing COLUMN order = AMD(AᵀA).
    sp::TripletBuilder<crd::f64> tb(&g_alloc, um, un);
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();
    auto acsc0 = sp::to_csc<crd::f64>(a, &g_alloc);
    auto ata = dir::ata_pattern(acsc0.pattern(), &g_alloc);
    auto perm = ord::amd_order(ata, &g_alloc);

    sp::TripletBuilder<crd::f64> pb(&g_alloc, um, un);
    pb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        pb.add(static_cast<crd::u32>(t.r), perm.inv_perm[static_cast<crd::u32>(t.c)], t.v);
    }
    auto ap = pb.compress();
    auto acsc = sp::to_csc<crd::f64>(ap, &g_alloc);
    const crd::containers::ConstSpan<crd::f64> apvals{acsc.values().values.data(), acsc.values().values.size()};

    // ---- Cerid MultifrontalQR ---- (also time the SYMBOLIC alone to split symbolic vs numeric)
    const crd::f64 t_cerid_sym =
        best_ms([&]() { auto s = dir::multifrontal_qr_symbolic(acsc.pattern(), &g_alloc); (void)s.nf(); });
    const crd::f64 t_cerid_fac = best_ms(
        [&]()
        {
            auto f = dir::factor_multifrontal_qr<crd::f64>(acsc.pattern(), apvals, &g_alloc);
            (void)f.info();
        });
    auto cf = dir::factor_multifrontal_qr<crd::f64>(acsc.pattern(), apvals, &g_alloc);

    // RHS b = A_p · 1 (length m; consistent LS recovers x = 1 over the n columns).
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
            acc += ap.values().values[st + k];
        }
        b[o] = acc;
    }
    crd::f64 t_cerid_slv = 0.0;
    crd::f64 cerid_res = 0.0;
    if (cf.info() == 0)
    {
        t_cerid_slv = best_ms([&]() { (void)cf.least_squares({b.data(), um}, {x.data(), un}, 1); });
        for (crd::u32 i = 0; i < un; ++i)
        {
            cerid_res += (x[i] - 1.0) * (x[i] - 1.0);
        }
        cerid_res = std::sqrt(cerid_res / static_cast<crd::f64>(un));
    }

    // ---- SPQR on the SAME permuted m×n matrix, NATURAL ordering, serial ----
    const crd::u32 innz = static_cast<crd::u32>(acsc.values().values.size());
    crd::containers::Array<int> cp(&g_alloc);
    crd::containers::Array<int> ci(&g_alloc);
    cp.resize(static_cast<crd::usize>(un) + 1);
    ci.resize(innz);
    for (crd::u32 o = 0; o <= un; ++o)
    {
        cp[o] = static_cast<int>(acsc.pattern().outer_ptr[o]);
    }
    for (crd::u32 k = 0; k < innz; ++k)
    {
        ci[k] = static_cast<int>(acsc.pattern().inner_idx[k]);
    }
    cholmod_sparse A;
    std::memset(&A, 0, sizeof(A));
    A.nrow = um;
    A.ncol = un;
    A.nzmax = innz;
    A.p = cp.data();
    A.i = ci.data();
    A.x = const_cast<crd::f64*>(acsc.values().values.data());
    A.stype = 0; // unsymmetric rectangular
    A.itype = CHOLMOD_INT;
    A.xtype = CHOLMOD_REAL;
    A.dtype = CHOLMOD_DOUBLE;
    A.sorted = 1;
    A.packed = 1;

    SuiteSparseQR_factorization<double, int>* QR = nullptr;
    const crd::f64 t_spqr_fac = best_ms(
        [&]()
        {
            if (QR != nullptr)
            {
                SuiteSparseQR_free<double, int>(&QR, cc);
            }
            QR = SuiteSparseQR_factorize<double, int>(SPQR_ORDERING_NATURAL, SPQR_NO_TOL, &A, cc);
        });
    const long long spqr_rank = (QR != nullptr) ? static_cast<long long>(QR->rank) : -1;

    crd::f64 t_spqr_slv = 0.0;
    crd::f64 spqr_res = -1.0;
    if (QR != nullptr)
    {
        cholmod_dense* Bd = cholmod_allocate_dense(um, 1, um, CHOLMOD_REAL, cc);
        auto* bx = static_cast<crd::f64*>(Bd->x);
        for (crd::u32 i = 0; i < um; ++i)
        {
            bx[i] = b[i];
        }
        cholmod_dense* Xd = nullptr;
        t_spqr_slv = best_ms(
            [&]()
            {
                if (Xd != nullptr)
                {
                    cholmod_free_dense(&Xd, cc);
                }
                cholmod_dense* Y = SuiteSparseQR_qmult<double, int>(SPQR_QTX, QR, Bd, cc); // Y = Qᵀ·b
                Xd = SuiteSparseQR_solve<double, int>(SPQR_RETX_EQUALS_B, QR, Y, cc);      // X = E·(R\Y)
                cholmod_free_dense(&Y, cc);
            });
        const auto* xx = static_cast<const crd::f64*>(Xd->x);
        crd::f64 acc = 0.0;
        for (crd::u32 i = 0; i < un; ++i)
        {
            acc += (xx[i] - 1.0) * (xx[i] - 1.0);
        }
        spqr_res = std::sqrt(acc / static_cast<crd::f64>(un));
        cholmod_free_dense(&Xd, cc);
        cholmod_free_dense(&Bd, cc);
    }

    const crd::f64 fac_ratio = t_cerid_fac > 0 ? t_spqr_fac / t_cerid_fac : 0.0; // >1 ⇒ Cerid faster
    const crd::f64 slv_ratio = t_cerid_slv > 0 ? t_spqr_slv / t_cerid_slv : 0.0;
    std::printf("  %-12s %5dx%-5d | FACTOR cerid=%9.3f (sym=%6.3f num=%6.3f) spqr=%9.3f ms (%.2fx %s) | SOLVE "
                "cerid=%7.3f spqr=%7.3f (%.2fx) | Rnnz_c=%-9llu rank_s=%-7lld | resid c=%.1e s=%.1e\n",
                name, mrows, ncols, t_cerid_fac, t_cerid_sym, t_cerid_fac - t_cerid_sym, t_spqr_fac, fac_ratio,
                (fac_ratio >= 1.0 ? "WIN" : "lose"), t_cerid_slv, t_spqr_slv, slv_ratio,
                static_cast<unsigned long long>(cf.r_nnz()), spqr_rank, cerid_res, spqr_res);
    std::fflush(stdout);

    if (QR != nullptr)
    {
        SuiteSparseQR_free<double, int>(&QR, cc);
    }
}
} // namespace

int main()
{
    cholmod_common cc;
    cholmod_start(&cc);
    cc.SPQR_grain = 1; // serial inter-front (Cerid v5c-1 QR is serial; run OPENBLAS_NUM_THREADS=1)
    std::printf("[bench_hesap_qr_vs_spqr] Cerid MultifrontalQR vs SuiteSparseQR/SPQR (NATURAL order on the same "
                "AMD(AᵀA)-permuted matrix, no rank detection, SERIAL). gate = residual.\n");
    std::printf("[rectangular least-squares — QR's home turf]\n");
    run("well1033", &cc);
    run("illc1033", &cc);
    run("well1850", &cc);
    run("illc1850", &cc);
    std::printf("[square structural]\n");
    run("bcsstk13", &cc);
    run("bcsstk24", &cc);
    cholmod_finish(&cc);
    return 0;
}
