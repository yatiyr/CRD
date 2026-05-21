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

#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
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
    std::uint32_t r, c;
    double        v;
};

enum class Pattern
{
    Uniform,
    Banded,
    PowerLaw
};

std::vector<Coord> generate(Pattern p, std::uint32_t n, std::uint32_t nnz_per_row, std::uint64_t seed)
{
    std::vector<Coord>  out;
    std::uint64_t       s = seed;
    auto                next = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(s >> 33);
    };
    auto push = [&](std::uint32_t r, std::uint32_t c) { out.push_back(Coord{r, c, 1.0 + static_cast<double>(next() % 7)}); };

    if (p == Pattern::Uniform)
    {
        out.reserve(static_cast<std::size_t>(n) * nnz_per_row);
        for (std::uint32_t r = 0; r < n; ++r)
        {
            for (std::uint32_t k = 0; k < nnz_per_row; ++k)
            {
                push(r, next() % n);
            }
        }
    }
    else if (p == Pattern::Banded)
    {
        const std::int32_t half = static_cast<std::int32_t>(nnz_per_row) / 2;
        out.reserve(static_cast<std::size_t>(n) * nnz_per_row);
        for (std::uint32_t r = 0; r < n; ++r)
        {
            for (std::int32_t d = -half; d <= half; ++d)
            {
                const std::int32_t c = static_cast<std::int32_t>(r) + d;
                if (c >= 0 && c < static_cast<std::int32_t>(n))
                {
                    push(r, static_cast<std::uint32_t>(c));
                }
            }
        }
    }
    else  // PowerLaw: ~90% rows have 2 nnz, ~10% have many.
    {
        for (std::uint32_t r = 0; r < n; ++r)
        {
            const std::uint32_t deg = (next() % 10 == 0) ? 64U : 2U;
            for (std::uint32_t k = 0; k < deg; ++k)
            {
                push(r, next() % n);
            }
        }
    }
    return out;
}

template <typename Fn>
double best_ms(Fn&& fn)
{
    fn();  // warmup (page-in y, warm caches/threads)
    double best = 1e30;
    for (int rep = 0; rep < 15; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* label, Pattern p, std::uint32_t n, std::uint32_t nnz_per_row)
{
    const auto coords = generate(p, n, nnz_per_row, 0xBEEFULL ^ n ^ (static_cast<std::uint64_t>(nnz_per_row) << 20));

    // Bench (app) scratch: malloc-backed allocator avoids a fixed-arena OOM at
    // 16M triplets. Allocation happens OUTSIDE the timed spmv regions.
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    crd::hesap::sparse::TripletBuilder<crd::f64> b(alloc, n, n);
    b.reserve(coords.size());
    for (const Coord& e : coords)
    {
        b.add(e.r, e.c, e.v);
    }
    auto csr  = b.compress();
    auto sell = crd::hesap::sparse::to_sell(csr, alloc);

    std::vector<double> x(n), y(n, 0.0);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        x[i] = 1.0 + static_cast<double>(i % 13) * 0.01;
    }
    crd::containers::ConstSpan<crd::f64> xs{x.data(), x.size()};
    crd::containers::Span<crd::f64>      ys{y.data(), y.size()};

    const double t_sell_st = best_ms([&]() { crd::hesap::sparse::spmv_sell<crd::f64>(1.0, sell, xs, 0.0, ys); });
    const double t_sell_mt = best_ms([&]() {
        crd::hesap::sparse::spmv_sell_parallel<crd::f64>(1.0, sell, xs, 0.0, ys);
        crd::jobs::frame_reset();  // reclaim the parallel_for JobDecl arena between reps
    });

    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(coords.size());
    for (const Coord& e : coords)
    {
        trips.emplace_back(static_cast<int>(e.r), static_cast<int>(e.c), e.v);
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> a(static_cast<int>(n), static_cast<int>(n));
    a.setFromTriplets(trips.begin(), trips.end());
    Eigen::Map<const Eigen::VectorXd> xv(x.data(), n);
    Eigen::VectorXd                   yv(n);

    Eigen::setNbThreads(1);
    const double t_eigen_st = best_ms([&]() { yv.noalias() = a * xv; });
    Eigen::setNbThreads(0);  // 0 => Eigen's default (all hardware threads)
    const double t_eigen_mt = best_ms([&]() { yv.noalias() = a * xv; });

    const double st = t_eigen_st / t_sell_st;
    const double mt = t_eigen_mt / t_sell_mt;
    std::printf("  %-18s N=%-8u nnz~%-3u | ST SELL=%7.3f Eigen=%7.3f =%.2fx %-4s | MT SELL=%7.3f Eigen=%7.3f =%.2fx %s\n",
                label, n, static_cast<unsigned>(coords.size() / n), t_sell_st, t_eigen_st, st,
                (st >= 1.0 ? "WIN" : "loss"), t_sell_mt, t_eigen_mt, mt, (mt >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_spmv] y=A*x: Cerid SELL (ST + MT) vs Eigen (ST + MT), best-of-15 + warmup, f64, N=2M. "
                "Gate: SELL/Eigen>=1 in both columns\n");
    for (std::uint32_t n : {1000000U, 2000000U})  // two cache regimes (x in L3 vs spilling)
    {
        run("uniform-16", Pattern::Uniform, n, 16);
        run("uniform-4", Pattern::Uniform, n, 4);
        run("banded-5", Pattern::Banded, n, 5);
        run("power-law", Pattern::PowerLaw, n, 0);
    }
    crd::jobs::shutdown();
    return 0;
}
