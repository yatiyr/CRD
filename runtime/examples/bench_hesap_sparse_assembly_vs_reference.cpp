// bench_hesap_sparse_assembly_vs_reference -- Phase 3.1.6 v1a-3.
//
// Head-to-head sparse ASSEMBLY throughput: Cerid TripletBuilder<f64> + compress()
// vs Eigen::SparseMatrix<double>::setFromTriplets. Both start from the same
// generated coordinate list and build their native triplet container + assemble
// the compressed CSR; we time fill+assemble end-to-end (best-of-3). This is the
// PETSc/Eigen "preallocate then compress" assembly path -- the v1a perf axis.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. Characterize-now (not a gate):
// reports the Eigen/Cerid speedup ratio; the hard perf gates begin at v1b (spmv).

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Sparse>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
using Clock = std::chrono::high_resolution_clock;

struct Coord
{
    std::uint32_t r;
    std::uint32_t c;
    double        v;
};

// Deterministic LCG so both libraries see identical coordinates.
std::vector<Coord> generate(std::uint32_t n, std::uint32_t nnz_per_row, std::uint64_t seed)
{
    std::vector<Coord> out;
    out.reserve(static_cast<std::size_t>(n) * nnz_per_row);
    std::uint64_t s = seed;
    auto next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(s >> 33);
    };
    for (std::uint32_t r = 0; r < n; ++r)
    {
        for (std::uint32_t k = 0; k < nnz_per_row; ++k)
        {
            const std::uint32_t c = next() % n;
            out.push_back(Coord{r, c, 1.0 + static_cast<double>(next() % 7)});
        }
    }
    return out;
}

double cerid_assemble_ms(const std::vector<Coord>& coords, std::uint32_t n)
{
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep)
    {
        crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(coords.size()) * 64 + (4u << 20));
        const auto t0 = Clock::now();
        crd::hesap::sparse::TripletBuilder<crd::f64> b(&alloc, n, n);
        b.reserve(coords.size());
        for (const Coord& e : coords)
        {
            b.add(e.r, e.c, e.v);
        }
        auto m = b.compress();
        const auto t1 = Clock::now();
        volatile crd::usize sink = m.nnz();
        (void)sink;
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

double eigen_assemble_ms(const std::vector<Coord>& coords, std::uint32_t n)
{
    double best = 1e30;
    for (int rep = 0; rep < 3; ++rep)
    {
        const auto t0 = Clock::now();
        std::vector<Eigen::Triplet<double>> trips;
        trips.reserve(coords.size());
        for (const Coord& e : coords)
        {
            trips.emplace_back(static_cast<int>(e.r), static_cast<int>(e.c), e.v);
        }
        Eigen::SparseMatrix<double> a(static_cast<int>(n), static_cast<int>(n));
        a.setFromTriplets(trips.begin(), trips.end());
        const auto t1 = Clock::now();
        volatile int sink = static_cast<int>(a.nonZeros());
        (void)sink;
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(std::uint32_t n, std::uint32_t nnz_per_row)
{
    const auto coords = generate(n, nnz_per_row, 0x1234567ULL ^ n);
    const double cerid = cerid_assemble_ms(coords, n);
    const double eigen = eigen_assemble_ms(coords, n);
    std::printf("  N=%-8u nnz/row=%-3u triplets=%-10zu  Cerid=%8.2f ms  Eigen=%8.2f ms  speedup=%.2fx %s\n", n,
                nnz_per_row, coords.size(), cerid, eigen, eigen / cerid, (eigen >= cerid ? "WIN" : "loss"));
}
} // namespace

int main()
{
    std::printf("[bench_hesap_sparse_assembly] Cerid compress() vs Eigen setFromTriplets (best-of-3, f64)\n");
    run(50000, 16);
    run(200000, 16);
    run(1000000, 8);
    return 0;
}
