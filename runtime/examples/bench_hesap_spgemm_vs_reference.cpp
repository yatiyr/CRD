// bench_hesap_spgemm_vs_reference -- Phase 3.1.6 v1d-2.
//
// C = A*A on REAL SuiteSparse matrices: Cerid serial + parallel spgemm vs
// Eigen (single-threaded -- Eigen does NOT multi-thread sparse*sparse). The
// v1d gate: median Cerid-parallel / Eigen >= 1.3x. Matrices are fetched at
// configure time (CMake file(DOWNLOAD), gated by CRD_BUILD_HESAP_VS_REFERENCE)
// into CRD_SUITESPARSE_DIR; we read the .mtx with a minimal Matrix-Market
// reader (the polished engine-side MM I/O + writer + CLI is v1g).
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Sparse>

#include <algorithm>
#include <chrono>
#include <cstdint>
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

// Minimal Matrix Market coordinate reader. Handles real/integer/pattern field
// and symmetric expansion. Returns triplets + dimension; success flag.
struct Mtx
{
    int               n = 0;  // square assumed (A*A); rows==cols
    std::vector<Trip> trips;
    bool              ok = false;
};

Mtx read_mtx(const std::string& path)
{
    Mtx          out;
    std::ifstream f(path);
    if (!f.is_open())
    {
        return out;
    }
    std::string line;
    if (!std::getline(f, line))
    {
        return out;
    }
    const bool pattern   = line.find("pattern") != std::string::npos;
    const bool symmetric = line.find("symmetric") != std::string::npos || line.find("hermitian") != std::string::npos;
    // skip comments
    do
    {
        if (!std::getline(f, line))
        {
            return out;
        }
    } while (!line.empty() && line[0] == '%');
    int rows = 0, cols = 0, nnz = 0;
    {
        std::istringstream hs(line);
        hs >> rows >> cols >> nnz;
    }
    out.n = rows < cols ? rows : cols;  // we square-pad below by using max dim
    out.n = rows > cols ? rows : cols;
    out.trips.reserve(static_cast<std::size_t>(nnz) * (symmetric ? 2 : 1));
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
        --c;  // MM is 1-indexed
        out.trips.push_back(Trip{r, c, v});
        if (symmetric && r != c)
        {
            out.trips.push_back(Trip{c, r, v});
        }
    }
    out.ok = true;
    return out;
}

template <typename Fn>
double best_ms(Fn&& fn)
{
    fn();  // warmup
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

void run(const char* name, const char* group)
{
    std::string path = std::string(CRD_SUITESPARSE_DIR) + "/" + name + "/" + name + ".mtx";
    Mtx         mtx  = read_mtx(path);
    if (!mtx.ok)
    {
        std::printf("  %-12s (%s)  SKIP (not found: %s)\n", name, group, path.c_str());
        return;
    }
    const int n = mtx.n;

    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();

    const double t_ser = best_ms([&]() {
        auto c = crd::hesap::sparse::spgemm(a, a, alloc);
        volatile crd::usize s = c.nnz();
        (void)s;
    });
    const double t_par = best_ms([&]() {
        auto c = crd::hesap::sparse::spgemm_parallel(a, a, alloc);
        crd::jobs::frame_reset();
        volatile crd::usize s = c.nnz();
        (void)s;
    });

    std::vector<Eigen::Triplet<double>> et;
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.emplace_back(t.r, t.c, t.v);
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(n, n);
    ea.setFromTriplets(et.begin(), et.end());
    Eigen::setNbThreads(1);
    Eigen::SparseMatrix<double, Eigen::RowMajor> ec;
    const double t_eig = best_ms([&]() {
        ec = ea * ea;  // natural Eigen idiom (no .pruned() — matches our no-prune behavior)
        volatile int s = static_cast<int>(ec.nonZeros());
        (void)s;
    });

    const double ratio = t_eig / t_par;
    std::printf("  %-12s nnz(A)=%-8lld nnz(C)=%-9lld | Cerid ser=%8.2f par=%8.2f  Eigen=%8.2f ms  par/Eigen=%.2fx %s\n",
                name, static_cast<long long>(a.nnz()), static_cast<long long>(ec.nonZeros()), t_ser, t_par, t_eig,
                ratio, (ratio >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_spgemm] C=A*A on SuiteSparse: Cerid serial/parallel vs Eigen-ST (best-of-7). "
                "Gate: par/Eigen>=1.3x median\n");
    run("bcsstk13", "HB");
    run("bcsstk24", "HB");
    run("bcsstk25", "HB");
    run("sherman3", "HB");
    run("gemat11", "HB");
    run("west2021", "HB");
    crd::jobs::shutdown();
    return 0;
}
