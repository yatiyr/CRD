// bench_hesap_lu_vs_reference -- Phase 3.1.6 v5b-1 (Gilbert-Peierls LU oracle bench).
//
// On real SuiteSparse matrices: factor + solve the SAME AMD-ordered matrix with
// (a) Cerid Gilbert-Peierls sparse LU (the SERIAL reference oracle) and (b)
// Eigen SparseLU<.., NaturalOrdering> (fed the already column-permuted matrix).
// Both dynamic-partial-pivot, so their pivot SEQUENCES (and thus fill) may
// DIVERGE — this is a RESIDUAL-correctness + perf-baseline comparison, NOT an
// identical-factor one. Reports factor/solve time, fill (nnz L+U), and residual.
//
// HONEST framing: v5b-1 is the SERIAL correctness oracle, NOT a crush. It is
// EXPECTED to lose to Eigen SparseLU on time (Eigen is a tuned production LU;
// this is a CSparse-faithful reference). The bench's value here is (1)
// CORRECTNESS — Cerid's residual matches Eigen's — and (2) a baseline the v5b-2
// supernodal + MC64 + threshold LU (parallel, deterministic) is measured against.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. crd conventions throughout;
// raw double only at the Eigen API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/sparse_lu.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/SparseCore>
#include <Eigen/SparseLU>
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

    // Cerid AMD ordering, then permute so both factorizations see the SAME ordered system.
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
    auto ap = pb.compress();              // permuted CSR
    auto acsc = sp::to_csc<crd::f64>(ap, &g_alloc); // CSC for the column-oriented GP-LU

    // ---- Cerid Gilbert-Peierls LU (SERIAL oracle) ----
    const crd::f64 t_cerid_fac = best_ms([&]() { auto f = dir::factor_gp_lu<crd::f64>(acsc, &g_alloc); (void)f.info(); });
    auto cf = dir::factor_gp_lu<crd::f64>(acsc, &g_alloc);

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
            acc += ap.values().values[st + k]; // row o · 1
        }
        b[o] = acc;
    }
    crd::f64 t_cerid_slv = 0.0;
    crd::f64 cerid_res = 0.0;
    crd::u64 cerid_fill = 0;
    if (cf.info() == 0)
    {
        t_cerid_slv = best_ms([&]() { for (crd::u32 i = 0; i < un; ++i) { x[i] = b[i]; } (void)cf.solve({x.data(), un}); });
        for (crd::u32 i = 0; i < un; ++i)
        {
            cerid_res += (x[i] - 1.0) * (x[i] - 1.0);
        }
        cerid_res = std::sqrt(cerid_res / static_cast<crd::f64>(un));
        cerid_fill = cf.factor_nnz();
    }

    // ---- Eigen SparseLU on the SAME permuted matrix, NaturalOrdering (column perm already applied) ----
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
    Eigen::SparseLU<Eigen::SparseMatrix<double>, Eigen::NaturalOrdering<int>> elu;
    const crd::f64 t_eig_fac = best_ms([&]() { elu.analyzePattern(ea); elu.factorize(ea); });
    Eigen::VectorXd eb(n);
    for (crd::u32 i = 0; i < un; ++i)
    {
        eb[static_cast<int>(i)] = b[i];
    }
    Eigen::VectorXd ex;
    const crd::f64 t_eig_slv = best_ms([&]() { ex = elu.solve(eb); });
    double eig_res = 0.0;
    if (elu.info() == Eigen::Success)
    {
        for (crd::u32 i = 0; i < un; ++i)
        {
            eig_res += (ex[static_cast<int>(i)] - 1.0) * (ex[static_cast<int>(i)] - 1.0);
        }
        eig_res = std::sqrt(eig_res / static_cast<double>(un));
    }
    const crd::u64 eig_fill =
        elu.info() == Eigen::Success ? static_cast<crd::u64>(elu.nnzL()) + static_cast<crd::u64>(elu.nnzU()) : 0;

    const char* cerid_ok = cf.info() == 0 ? "ok" : "SINGULAR";
    const double fac_ratio = (t_cerid_fac > 0.0) ? t_eig_fac / t_cerid_fac : 0.0; // >1 ⇒ Cerid faster
    const double slv_ratio = (t_cerid_slv > 0.0) ? t_eig_slv / t_cerid_slv : 0.0;
    std::printf("  %-10s n=%-7d | FACTOR cerid=%9.2f eigen=%9.2f ms (%.2fx) | SOLVE cerid=%7.3f eigen=%7.3f (%.2fx) "
                "| fill cerid=%-9llu eigen=%-9llu | resid c=%.1e e=%.1e [%s]\n",
                name, n, t_cerid_fac, t_eig_fac, fac_ratio, t_cerid_slv, t_eig_slv, slv_ratio,
                static_cast<unsigned long long>(cerid_fill), static_cast<unsigned long long>(eig_fill), cerid_res,
                eig_res, cerid_ok);
    std::fflush(stdout); // flush per-matrix so progress is visible (and a hang shows which matrix)
}
} // namespace

int main()
{
    std::printf("[bench_hesap_lu_vs_reference] Cerid Gilbert-Peierls LU (SERIAL oracle) vs Eigen SparseLU "
                "(NaturalOrdering on the same AMD-permuted matrix). Oracle: expect to LOSE on time; gate = residual.\n");
    // SMALL corpus ONLY. v5b-1 has NO column reorder (deferred to v5b-2) and is a serial left-looking oracle;
    // an AMD-SYMMETRIC ordering does not bound LU fill the way COLAMD would, so 3D-FEM fill blows up fast —
    // even bcsstk25 (15k) balloons to multi-GB (measured). bcsstk13 (2k) / bcsstk24 (3.5k) stay manageable.
    // bcsstk25 + hood (220k) + ldoor (1M) are v5b-2's turf (supernodal + COLAMD + parallel).
    run("bcsstk13");
    run("bcsstk24");
    return 0;
}
