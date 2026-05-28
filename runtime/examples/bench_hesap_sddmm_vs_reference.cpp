// bench_hesap_sddmm_vs_reference -- Phase 3.1.6 v1e-2.
//
// SDDMM: C = sample(X*Y^T, M) -- C[i,j] = dot(X[i,:], Y[j,:]) for (i,j) in
// pattern(M). Mask M = a REAL SuiteSparse matrix's sparsity pattern; X, Y are
// dense (n x r). Eigen has NO SDDMM op, so we compare against the two ways an
// Eigen user would actually compute it:
//
//   (1) SAME-FLOPS kernel race (the honest gate): loop M's nonzeros and take
//       each X.row(i).dot(Y.row(j)) with Eigen's vectorised dot. Identical
//       FLOPs to ours -- tests row-parallelism + cache layout, not asymptotics.
//   (2) DENSE-then-mask (what you'd write without a kernel): form the full
//       dense D = X*Y^T (O(n^2 r)) then read masked entries. We compute only
//       nnz(M) dots -> structural win. Memory-capped (n^2 doubles) so it only
//       runs on the smaller matrices.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. Matrices fetched at
// configure (CRD_SUITESPARSE_DIR), read with a crd-native Matrix-Market reader.
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named GrowableTlsfAllocator (never malloc / default_allocator); crd::f64/usize/
// i32/u32 (never raw). Raw double only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib> // strtol / strtod
#include <cstring> // strncmp

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace fs = crd::platform::fs;

crd::memory::GrowableTlsfAllocator g_alloc;

struct Trip
{
    crd::i32 r, c;
    crd::f64 v;
};

// crd-native Matrix-Market reader (read_file_text + strtol/strtod; pattern/symmetric/hermitian).
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

template <typename Fn> crd::f64 best_ms(Fn&& fn)
{
    fn();
    crd::f64 best = 1e30;
    for (crd::i32 rep = 0; rep < 7; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* name, crd::i32 r)
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
        std::printf("  %-10s r=%-4d SKIP (not found)\n", name, r);
        return;
    }

    crd::memory::IAllocator* alloc = &g_alloc;
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto mask = tb.compress(); // mask = the SuiteSparse pattern
    const auto ru = static_cast<crd::u32>(r);

    // Dense X (n x r), Y (n x r), row-major.
    crd::containers::Array<crd::f64> x(alloc);
    crd::containers::Array<crd::f64> y(alloc);
    x.resize(static_cast<crd::usize>(n) * r);
    y.resize(static_cast<crd::usize>(n) * r);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        x[i] = 1.0 + 0.01 * static_cast<crd::f64>(i % 31);
        y[i] = 0.5 + 0.02 * static_cast<crd::f64>(i % 17);
    }

    // ---- Cerid SDDMM (serial + parallel) ----
    crd::hesap::sparse::SparseMatrix<crd::f64, crd::hesap::sparse::SparseFormat::Csr> cser{
        crd::hesap::sparse::SparsePattern{alloc}, crd::hesap::sparse::SparseValues<crd::f64>{alloc}};
    const crd::f64 t_ser = best_ms(
        [&]() { cser = crd::hesap::sparse::sddmm<crd::f64>(mask, x.data(), ru, y.data(), ru, ru, 1.0, alloc); });
    const crd::f64 t_par = best_ms(
        [&]()
        {
            auto cc = crd::hesap::sparse::sddmm_parallel<crd::f64>(mask, x.data(), ru, y.data(), ru, ru, 1.0, alloc);
            crd::jobs::frame_reset();
            (void)cc;
        });

    // ---- Eigen baseline (1): same-flops per-entry vectorised dot ----
    using RowMat = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
    Eigen::Map<RowMat> ex(x.data(), n, r);
    Eigen::Map<RowMat> ey(y.data(), n, r);
    const crd::u32* outer = mask.pattern().outer_ptr.data();
    const crd::u32* inner = mask.pattern().inner_idx.data();
    const crd::u32 m = static_cast<crd::u32>(n);
    crd::containers::Array<crd::f64> eout(alloc);
    eout.resize(mask.nnz());
    const crd::f64 t_eig_dot = best_ms(
        [&]()
        {
            for (crd::u32 i = 0; i < m; ++i)
            {
                for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
                {
                    eout[k] = ex.row(i).dot(ey.row(inner[k]));
                }
            }
        });

    // Correctness: Cerid serial vs Eigen per-entry dot.
    crd::f64 max_diff = 0.0;
    for (crd::usize k = 0; k < cser.nnz(); ++k)
    {
        max_diff = std::max(max_diff, std::abs(cser.values().values[k] - eout[k]));
    }

    const crd::f64 dot_ratio = t_eig_dot / t_par; // >1 => Cerid wins same-flops race

    // ---- Eigen baseline (2): dense X*Y^T then mask (memory-capped) ----
    char dense_note[64] = "dense-mask: SKIP (n^2 too large)";
    const crd::f64 dense_bytes = static_cast<crd::f64>(n) * static_cast<crd::f64>(n) * 8.0;
    if (dense_bytes <= 512.0 * 1024.0 * 1024.0) // <= 512 MB dense product
    {
        const crd::f64 t_eig_dense = best_ms(
            [&]()
            {
                RowMat d = ex * ey.transpose(); // O(n^2 r) -- the whole point we avoid
                volatile double sink = 0.0;
                for (crd::u32 i = 0; i < m; ++i)
                {
                    for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
                    {
                        sink += d(i, inner[k]);
                    }
                }
                (void)sink;
            });
        std::snprintf(dense_note, sizeof(dense_note), "dense-mask=%9.3f ms (%.0fx our par)", t_eig_dense,
                      t_eig_dense / t_par);
    }

    std::printf("  %-10s r=%-4d | Cerid ser=%8.3f par=%8.3f  Eigen dot=%8.3f ms  dot/our-par=%.2fx %s  | %s  "
                "[maxdiff=%.1e]\n",
                name, r, t_ser, t_par, t_eig_dot, dot_ratio, (dot_ratio >= 1.0 ? "WIN" : "loss"), dense_note, max_diff);
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_sddmm] C=sample(X*Y^T, mask) on SuiteSparse: Cerid vs Eigen per-entry dot (same flops, "
                "best-of-7) + dense-then-mask illustration.\n");
    for (crd::i32 r : {8, 16, 32})
    {
        run("bcsstk24", r);
        run("gemat11", r);
        run("sherman3", r);
    }
    crd::jobs::shutdown();
    return 0;
}
