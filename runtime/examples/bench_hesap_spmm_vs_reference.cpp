// bench_hesap_spmm_vs_reference -- Phase 3.1.6 v1e-1.
//
// C = A*B (sparse A x dense B, multiple RHS r) on REAL SuiteSparse matrices:
// Cerid serial + parallel vs Eigen sparse*dense (ST + MT). Honest framing:
// Eigen's sparse*dense is ALSO one-pass-over-A; the win is row-PARALLELISM, so
// the gate (>=2x) is meaningful vs Eigen-MT. Eigen + Cerid both row-major dense.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. Matrices fetched at configure
// (CRD_SUITESPARSE_DIR), read with a minimal Matrix-Market reader.

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Sparse>

#include <algorithm>
#include <chrono>
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
    auto a = tb.compress();

    std::vector<double> b(static_cast<std::size_t>(n) * r), c(static_cast<std::size_t>(n) * r);
    for (std::size_t i = 0; i < b.size(); ++i)
    {
        b[i] = 1.0 + 0.01 * static_cast<double>(i % 31);
    }
    const auto ru = static_cast<crd::u32>(r);

    const double t_ser = best_ms([&]() { crd::hesap::sparse::spmm<crd::f64>(1.0, a, b.data(), ru, ru, 0.0, c.data(), ru); });
    const double t_par = best_ms([&]() {
        crd::hesap::sparse::spmm_parallel<crd::f64>(1.0, a, b.data(), ru, ru, 0.0, c.data(), ru);
        crd::jobs::frame_reset();
    });

    std::vector<Eigen::Triplet<double>> et;
    et.reserve(trips.size());
    for (const Trip& t : trips)
    {
        et.emplace_back(t.r, t.c, t.v);
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(n, n);
    ea.setFromTriplets(et.begin(), et.end());
    using RowMat = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
    RowMat eb = Eigen::Map<RowMat>(b.data(), n, r);
    RowMat ec(n, r);

    Eigen::setNbThreads(1);
    const double t_eig_st = best_ms([&]() { ec.noalias() = ea * eb; });
    Eigen::setNbThreads(0);
    const double t_eig_mt = best_ms([&]() { ec.noalias() = ea * eb; });

    const double mt = t_eig_mt / t_par;
    std::printf("  %-10s r=%-4d | Cerid ser=%8.3f par=%8.3f  Eigen ST=%8.3f MT=%8.3f ms  par/Eigen-MT=%.2fx %s\n", name,
                r, t_ser, t_par, t_eig_st, t_eig_mt, mt, (mt >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_spmm] C=A*B (sparse x dense, r RHS) on SuiteSparse: Cerid vs Eigen (best-of-7). "
                "Gate: par/Eigen-MT>=2x on r>=4\n");
    for (int r : {1, 4, 32, 128})
    {
        run("bcsstk25", r);
        run("gemat11", r);
    }
    crd::jobs::shutdown();
    return 0;
}
