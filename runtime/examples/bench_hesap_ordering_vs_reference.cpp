// bench_hesap_ordering_vs_reference -- Phase 3.1.6 v2a.
//
// On real SuiteSparse SPD matrices (bcsstk* FEM):
//   (1) VALIDATE the symbolic-Cholesky port: our nnz_l(natural) MUST equal
//       Eigen SimplicialLLT<Lower, NaturalOrdering>.matrixL().nonZeros().
//   (2) RCM fill/bandwidth reduction (our nnz_l + bandwidth, natural vs RCM).
//   (3) Set the v2b AMD target: Eigen SimplicialLLT<Lower, AMDOrdering> nnz(L)
//       — the number our AMD (v2b) must reach (≤ 1.05×) + beat on time.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.

#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/OrderingMethods>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

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
namespace sp  = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;

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
double best_ms(Fn&& fn, int reps = 5)
{
    fn();
    double best = 1e30;
    for (int r = 0; r < reps; ++r)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* name)
{
    std::string       path = std::string(CRD_SUITESPARSE_DIR) + "/" + name + "/" + name + ".mtx";
    int               n    = 0;
    std::vector<Trip> trips;
    if (!read_mtx(path, n, trips))
    {
        std::printf("  %-10s SKIP (not found)\n", name);
        return;
    }

    crd::memory::IAllocator*     alloc = crd::memory::default_allocator();
    sp::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();

    // Cerid: nnz(L) natural + RCM, bandwidth natural + RCM, RCM time.
    const crd::u64 nnzl_nat = ord::nnz_l(a.pattern(), alloc);
    const crd::u32 bw_nat   = ord::bandwidth(a.pattern());
    crd::u64       nnzl_rcm = 0;
    crd::u32       bw_rcm   = 0;
    const double   t_rcm    = best_ms([&]() {
        auto p  = ord::rcm_order(a.pattern(), alloc);
        auto rp = ord::apply_symmetric(a.pattern(), p, alloc);
        nnzl_rcm = ord::nnz_l(rp, alloc);
        bw_rcm   = ord::bandwidth(rp);
    });

    // Cerid AMD (v2b): ordering time alone, then its fill.
    ord::Permutation amdp(alloc);
    const double     t_amd = best_ms([&]() { amdp = ord::amd_order(a.pattern(), alloc); });
    const crd::u64   nnzl_amd = ord::nnz_l(ord::apply_symmetric(a.pattern(), amdp, alloc), alloc);

    // Eigen: build symmetric SparseMatrix (lower triangle drives LLT).
    std::vector<Eigen::Triplet<double>> et;
    et.reserve(trips.size());
    for (const Trip& t : trips)
    {
        et.emplace_back(t.r, t.c, t.v);
    }
    Eigen::SparseMatrix<double> ea(n, n);
    ea.setFromTriplets(et.begin(), et.end());

    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::NaturalOrdering<int>> llt_nat;
    llt_nat.compute(ea);
    long long eig_nat = -1;
    if (llt_nat.info() == Eigen::Success)
    {
        const Eigen::SparseMatrix<double> l_nat = llt_nat.matrixL();  // materialise TriangularView
        eig_nat                                 = l_nat.nonZeros();
    }

    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>, Eigen::Lower, Eigen::AMDOrdering<int>> llt_amd;
    llt_amd.compute(ea);
    long long eig_amd = -1;
    if (llt_amd.info() == Eigen::Success)
    {
        const Eigen::SparseMatrix<double> l_amd = llt_amd.matrixL();
        eig_amd                                 = l_amd.nonZeros();
    }

    const bool   port_ok   = (eig_nat < 0) || (static_cast<long long>(nnzl_nat) == eig_nat);
    const double amd_ratio = eig_amd > 0 ? static_cast<double>(nnzl_amd) / static_cast<double>(eig_amd) : 0.0;
    std::printf("  %-10s n=%-6d | nnz(L) nat ours=%-9llu Eigen=%-9lld %s | RCM=%-9llu bw %u->%u | "
                "Eigen-AMD=%-9lld | OUR AMD=%-9llu (%.3fx Eigen-AMD %s) ord=%.2f ms\n",
                name, n, static_cast<unsigned long long>(nnzl_nat), eig_nat, (port_ok ? "MATCH" : "MISMATCH!"),
                static_cast<unsigned long long>(nnzl_rcm), bw_nat, bw_rcm, eig_amd,
                static_cast<unsigned long long>(nnzl_amd), amd_ratio,
                (amd_ratio > 0.0 && amd_ratio <= 1.05 ? "GATE-OK" : "OVER"), t_amd);
}
} // namespace

int main()
{
    std::printf("[bench_hesap_ordering] symbolic-Cholesky port validation + RCM fill/bandwidth + v2b AMD target "
                "(SuiteSparse SPD).\n");
    run("bcsstk13");
    run("bcsstk24");
    run("bcsstk25");
    return 0;
}
