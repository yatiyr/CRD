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
// configure (CRD_SUITESPARSE_DIR), read with a minimal Matrix-Market reader.

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace
{
using Clock = std::chrono::high_resolution_clock;

struct Trip
{
    int    r, c;
    double v;
};

bool read_mtx(const std::string& path, int& n, std::vector<Trip>& trips)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        return false;
    }
    std::string line;
    if (!std::getline(f, line))
    {
        return false;
    }
    const bool pattern   = line.find("pattern") != std::string::npos;
    const bool symmetric = line.find("symmetric") != std::string::npos || line.find("hermitian") != std::string::npos;
    do
    {
        if (!std::getline(f, line))
        {
            return false;
        }
    } while (!line.empty() && line[0] == '%');
    int rows = 0, cols = 0, nnz = 0;
    {
        std::istringstream hs(line);
        hs >> rows >> cols >> nnz;
    }
    n = rows > cols ? rows : cols;
    trips.reserve(static_cast<std::size_t>(nnz) * (symmetric ? 2 : 1));
    for (int e = 0; e < nnz; ++e)
    {
        if (!std::getline(f, line))
        {
            break;
        }
        std::istringstream ls(line);
        int                r = 0, c = 0;
        double             v = 1.0;
        ls >> r >> c;
        if (!pattern)
        {
            ls >> v;
        }
        --r;
        --c;
        trips.push_back(Trip{r, c, v});
        if (symmetric && r != c)
        {
            trips.push_back(Trip{c, r, v});
        }
    }
    return true;
}

template <typename Fn>
double best_ms(Fn&& fn)
{
    fn();
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* name, int r)
{
    std::string       path = std::string(CRD_SUITESPARSE_DIR) + "/" + name + "/" + name + ".mtx";
    int               n    = 0;
    std::vector<Trip> trips;
    if (!read_mtx(path, n, trips))
    {
        std::printf("  %-10s r=%-4d SKIP (not found)\n", name, r);
        return;
    }

    crd::memory::IAllocator*                     alloc = crd::memory::default_allocator();
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto       mask = tb.compress();  // mask = the SuiteSparse pattern
    const auto ru   = static_cast<crd::u32>(r);

    // Dense X (n x r), Y (n x r), row-major.
    std::vector<double> x(static_cast<std::size_t>(n) * r), y(static_cast<std::size_t>(n) * r);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        x[i] = 1.0 + 0.01 * static_cast<double>(i % 31);
        y[i] = 0.5 + 0.02 * static_cast<double>(i % 17);
    }

    // ---- Cerid SDDMM (serial + parallel) ----
    crd::hesap::sparse::SparseMatrix<crd::f64, crd::hesap::sparse::SparseFormat::Csr> cser{
        crd::hesap::sparse::SparsePattern{alloc}, crd::hesap::sparse::SparseValues<crd::f64>{alloc}};
    const double t_ser = best_ms([&]() {
        cser = crd::hesap::sparse::sddmm<crd::f64>(mask, x.data(), ru, y.data(), ru, ru, 1.0, alloc);
    });
    const double t_par = best_ms([&]() {
        auto cc = crd::hesap::sparse::sddmm_parallel<crd::f64>(mask, x.data(), ru, y.data(), ru, ru, 1.0, alloc);
        crd::jobs::frame_reset();
        (void)cc;
    });

    // ---- Eigen baseline (1): same-flops per-entry vectorised dot ----
    using RowMat = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
    Eigen::Map<RowMat>  ex(x.data(), n, r);
    Eigen::Map<RowMat>  ey(y.data(), n, r);
    const crd::u32*     outer = mask.pattern().outer_ptr.data();
    const crd::u32*     inner = mask.pattern().inner_idx.data();
    const crd::u32      m     = static_cast<crd::u32>(n);
    std::vector<double> eout(mask.nnz());
    const double        t_eig_dot = best_ms([&]() {
        for (crd::u32 i = 0; i < m; ++i)
        {
            for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
            {
                eout[k] = ex.row(i).dot(ey.row(inner[k]));
            }
        }
    });

    // Correctness: Cerid serial vs Eigen per-entry dot.
    double max_diff = 0.0;
    for (crd::usize k = 0; k < cser.nnz(); ++k)
    {
        max_diff = std::max(max_diff, std::abs(cser.values().values[k] - eout[k]));
    }

    const double dot_ratio = t_eig_dot / t_par;  // >1 => Cerid wins same-flops race

    // ---- Eigen baseline (2): dense X*Y^T then mask (memory-capped) ----
    char dense_note[64] = "dense-mask: SKIP (n^2 too large)";
    const double dense_bytes = static_cast<double>(n) * static_cast<double>(n) * 8.0;
    if (dense_bytes <= 512.0 * 1024.0 * 1024.0)  // <= 512 MB dense product
    {
        const double t_eig_dense = best_ms([&]() {
            RowMat d = ex * ey.transpose();  // O(n^2 r) -- the whole point we avoid
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
    for (int r : {8, 16, 32})
    {
        run("bcsstk24", r);
        run("gemat11", r);
        run("sherman3", r);
    }
    crd::jobs::shutdown();
    return 0;
}
