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
//
// crd conventions throughout: crd::containers::Array (never std::vector), named
// allocators (never malloc) — a GrowableTlsfAllocator for the shared corpus, and
// the existing per-rep TlsfAllocator for the timed Cerid build; crd::f64/u32/usize
// (never raw). Raw double only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;

// Shared corpus arena (coords + Eigen triplets); unbounded so 1M×16 fits.
crd::memory::GrowableTlsfAllocator g_alloc;

struct Coord
{
    crd::u32 r;
    crd::u32 c;
    crd::f64 v;
};

// Deterministic LCG so both libraries see identical coordinates.
crd::containers::Array<Coord> generate(crd::u32 n, crd::u32 nnz_per_row, crd::u64 seed)
{
    crd::containers::Array<Coord> out(&g_alloc);
    out.reserve(static_cast<crd::usize>(n) * nnz_per_row);
    crd::u64 s = seed;
    auto next = [&]()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 k = 0; k < nnz_per_row; ++k)
        {
            const crd::u32 c = next() % n;
            out.push_back(Coord{r, c, 1.0 + static_cast<crd::f64>(next() % 7)});
        }
    }
    return out;
}

crd::f64 cerid_assemble_ms(const crd::containers::Array<Coord>& coords, crd::u32 n)
{
    crd::f64 best = 1e30;
    for (crd::i32 rep = 0; rep < 3; ++rep)
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
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

crd::f64 eigen_assemble_ms(const crd::containers::Array<Coord>& coords, crd::u32 n)
{
    crd::f64 best = 1e30;
    for (crd::i32 rep = 0; rep < 3; ++rep)
    {
        const auto t0 = Clock::now();
        crd::containers::Array<Eigen::Triplet<double>> trips(&g_alloc);
        trips.reserve(coords.size());
        for (const Coord& e : coords)
        {
            trips.push_back(Eigen::Triplet<double>(static_cast<int>(e.r), static_cast<int>(e.c), e.v));
        }
        Eigen::SparseMatrix<double> a(static_cast<int>(n), static_cast<int>(n));
        a.setFromTriplets(trips.data(), trips.data() + trips.size());
        const auto t1 = Clock::now();
        volatile int sink = static_cast<int>(a.nonZeros());
        (void)sink;
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(crd::u32 n, crd::u32 nnz_per_row)
{
    const auto coords = generate(n, nnz_per_row, 0x1234567ULL ^ n);
    const crd::f64 cerid = cerid_assemble_ms(coords, n);
    const crd::f64 eigen = eigen_assemble_ms(coords, n);
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
