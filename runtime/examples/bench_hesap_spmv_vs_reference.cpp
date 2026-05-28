// bench_hesap_spmv_vs_reference -- Phase 3.1.6 v1b-1 / v1b-2.
//
// Single-thread y = A*x: Cerid CSR-scalar baseline + Cerid SELL-C-σ primary vs
// Eigen RowMajor SparseMatrix * dense vector, identical pattern. The v1b-2 gate
// is SELL-ST >= Eigen-ST on uniform-4 + banded-5 + uniform-16 (the cases SELL is
// structurally faster or bandwidth-tied); power-law may lose until σ (v1b-2-sigma).
// Eigen-ST spmv is largely memory-bandwidth bound -> SELL wins clearly on
// short/banded rows, ties/modestly-wins where bandwidth dominates.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
//
// crd conventions throughout: crd::containers::Array (never std::vector), a named
// GrowableTlsfAllocator (never malloc / default_allocator — the 16M-triplet working
// set exceeds one TLSF pool); crd::f64/u32/u64/i32/usize (never raw). Raw double
// only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;

// Unbounded pooled arena (N = 2M rows × ~16/row ≈ 32M triplets ≫ one 4 GB TLSF pool).
crd::memory::GrowableTlsfAllocator g_alloc;

struct Coord
{
    crd::u32 r, c;
    crd::f64 v;
};

enum class Pattern
{
    Uniform,
    Banded,
    PowerLaw
};

crd::containers::Array<Coord> generate(Pattern p, crd::u32 n, crd::u32 nnz_per_row, crd::u64 seed)
{
    crd::containers::Array<Coord> out(&g_alloc);
    crd::u64 s = seed;
    auto next = [&]()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(s >> 33);
    };
    auto push = [&](crd::u32 r, crd::u32 c)
    {
        out.push_back(Coord{r, c, 1.0 + static_cast<crd::f64>(next() % 7)});
    };

    if (p == Pattern::Uniform)
    {
        out.reserve(static_cast<crd::usize>(n) * nnz_per_row);
        for (crd::u32 r = 0; r < n; ++r)
        {
            for (crd::u32 k = 0; k < nnz_per_row; ++k)
            {
                push(r, next() % n);
            }
        }
    }
    else if (p == Pattern::Banded)
    {
        const crd::i32 half = static_cast<crd::i32>(nnz_per_row) / 2;
        out.reserve(static_cast<crd::usize>(n) * nnz_per_row);
        for (crd::u32 r = 0; r < n; ++r)
        {
            for (crd::i32 d = -half; d <= half; ++d)
            {
                const crd::i32 c = static_cast<crd::i32>(r) + d;
                if (c >= 0 && c < static_cast<crd::i32>(n))
                {
                    push(r, static_cast<crd::u32>(c));
                }
            }
        }
    }
    else // PowerLaw: ~90% rows have 2 nnz, ~10% have many.
    {
        for (crd::u32 r = 0; r < n; ++r)
        {
            const crd::u32 deg = (next() % 10 == 0) ? 64U : 2U;
            for (crd::u32 k = 0; k < deg; ++k)
            {
                push(r, next() % n);
            }
        }
    }
    return out;
}

template <typename Fn> crd::f64 best_ms(Fn&& fn)
{
    fn(); // warmup (page-in y, warm caches/threads)
    crd::f64 best = 1e30;
    for (crd::i32 rep = 0; rep < 15; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* label, Pattern p, crd::u32 n, crd::u32 nnz_per_row)
{
    const auto coords = generate(p, n, nnz_per_row, 0xBEEFULL ^ n ^ (static_cast<crd::u64>(nnz_per_row) << 20));

    crd::memory::IAllocator* alloc = &g_alloc;
    crd::hesap::sparse::TripletBuilder<crd::f64> b(alloc, n, n);
    b.reserve(coords.size());
    for (const Coord& e : coords)
    {
        b.add(e.r, e.c, e.v);
    }
    auto csr = b.compress();
    auto sell = crd::hesap::sparse::to_sell(csr, alloc);

    crd::containers::Array<crd::f64> x(alloc);
    crd::containers::Array<crd::f64> y(alloc);
    x.resize(n);
    y.resize(n, 0.0);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = 1.0 + static_cast<crd::f64>(i % 13) * 0.01;
    }
    crd::containers::ConstSpan<crd::f64> xs{x.data(), x.size()};
    crd::containers::Span<crd::f64> ys{y.data(), y.size()};

    const crd::f64 t_sell_st = best_ms([&]() { crd::hesap::sparse::spmv_sell<crd::f64>(1.0, sell, xs, 0.0, ys); });
    const crd::f64 t_sell_mt = best_ms(
        [&]()
        {
            crd::hesap::sparse::spmv_sell_parallel<crd::f64>(1.0, sell, xs, 0.0, ys);
            crd::jobs::frame_reset(); // reclaim the parallel_for JobDecl arena between reps
        });

    crd::containers::Array<Eigen::Triplet<double>> trips(alloc);
    trips.reserve(coords.size());
    for (const Coord& e : coords)
    {
        trips.push_back(Eigen::Triplet<double>(static_cast<int>(e.r), static_cast<int>(e.c), e.v));
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> a(static_cast<int>(n), static_cast<int>(n));
    a.setFromTriplets(trips.data(), trips.data() + trips.size());
    Eigen::Map<const Eigen::VectorXd> xv(x.data(), n);
    Eigen::VectorXd yv(n);

    Eigen::setNbThreads(1);
    const crd::f64 t_eigen_st = best_ms([&]() { yv.noalias() = a * xv; });
    Eigen::setNbThreads(0); // 0 => Eigen's default (all hardware threads)
    const crd::f64 t_eigen_mt = best_ms([&]() { yv.noalias() = a * xv; });

    const crd::f64 st = t_eigen_st / t_sell_st;
    const crd::f64 mt = t_eigen_mt / t_sell_mt;
    std::printf(
        "  %-18s N=%-8u nnz~%-3llu | ST SELL=%7.3f Eigen=%7.3f =%.2fx %-4s | MT SELL=%7.3f Eigen=%7.3f =%.2fx %s\n",
        label, n, static_cast<unsigned long long>(coords.size() / n), t_sell_st, t_eigen_st, st,
        (st >= 1.0 ? "WIN" : "loss"), t_sell_mt, t_eigen_mt, mt, (mt >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_spmv] y=A*x: Cerid SELL (ST + MT) vs Eigen (ST + MT), best-of-15 + warmup, f64, N=2M. "
                "Gate: SELL/Eigen>=1 in both columns\n");
    for (crd::u32 n : {1000000U, 2000000U}) // two cache regimes (x in L3 vs spilling)
    {
        run("uniform-16", Pattern::Uniform, n, 16);
        run("uniform-4", Pattern::Uniform, n, 4);
        run("banded-5", Pattern::Banded, n, 5);
        run("power-law", Pattern::PowerLaw, n, 0);
    }
    crd::jobs::shutdown();
    return 0;
}
