// bench_hesap_spmv_vs_reference — the SELL-C-σ spmv board (SIMD-DISPATCH; scripts/run_bench_spmv.sh, at each CPU
// level). y = A x for f64, f32, Complex<f64> and Complex<f32> over four patterns, at sizes the run derives from the
// host's caches (half the L2, between the L2 and the L3, half the L3) and two DRAM sizes, against Eigen's RowMajor
// SparseMatrix times a dense vector on the identical matrix (its peer library at the host's best ISA,
// examples/peers/eigen_peer.hpp).
//
// --one-thread: the hesap-sparse level's arms on one thread, timed round by round in turn so they share the host's
// state: the production spmv_sell; the forced U32 and Delta16 column forms (the Delta16 Auto cut's arms); the level's
// policy forced Resident and Streaming (the sweep cut's); and every scalar-load and gather form at pair widths 1, 8,
// 16, 32 and none (SellPick, refused n/a where the level has no gather), with Eigen on one thread. --threads: the
// U32 and Delta16 builds at each job's cut (SellJobCut: the whole L2, the CPU's L2 share) at 1, 4, 8, 16 and 32 jobs,
// then Eigen on as many OpenMP threads in a block of its own (libgomp's idle threads spin after a region). No
// argument runs both.
//
// Every Cerid arm must give the CSR spmv's bits (D(sparse)-3), Eigen within 4 (len + 2) eps of the row's sum of
// |a| |x|; a miss, or a refusal where the level gathers, fails the run.
//
// crd::containers::Array and one GrowableTlsfAllocator (the 2M-row working set exceeds one TLSF pool), never std
// containers or malloc.

#include "eigen_peer.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sell_parallel.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace
{
namespace sp = crd::hesap::sparse;
namespace peer = crd_peer::eigen;
using crd::f32;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::hesap::Complex;
using Clock = std::chrono::steady_clock;

crd::memory::GrowableTlsfAllocator g_alloc;
bool g_failed = false;

constexpr int kRounds = 15;
constexpr u32 kJobs[] = {1U, 4U, 8U, 16U, 32U};
constexpr usize kJobCounts = sizeof(kJobs) / sizeof(kJobs[0]);

template <typename T> using Part = sp::detail::SellScalarT<T>;
template <typename T> inline constexpr bool kComplex = !std::is_same_v<T, Part<T>>;

template <typename T> T scalar(f64 re, f64 im)
{
    if constexpr (kComplex<T>)
    {
        return T{static_cast<Part<T>>(re), static_cast<Part<T>>(im)};
    }
    else
    {
        (void)im;
        return static_cast<T>(re);
    }
}

template <typename T> const char* type_name()
{
    if constexpr (std::is_same_v<T, f64>)
    {
        return "f64";
    }
    else if constexpr (std::is_same_v<T, f32>)
    {
        return "f32";
    }
    else if constexpr (std::is_same_v<Part<T>, f64>)
    {
        return "c64";
    }
    else
    {
        return "c32";
    }
}

template <typename T> f64 magnitude(const T& v)
{
    if constexpr (kComplex<T>)
    {
        return std::hypot(static_cast<f64>(v.re), static_cast<f64>(v.im));
    }
    else
    {
        return std::fabs(static_cast<f64>(v));
    }
}

template <typename T> f64 distance(const T& a, const T& b)
{
    if constexpr (kComplex<T>)
    {
        return std::hypot(static_cast<f64>(a.re) - static_cast<f64>(b.re),
                          static_cast<f64>(a.im) - static_cast<f64>(b.im));
    }
    else
    {
        return std::fabs(static_cast<f64>(a) - static_cast<f64>(b));
    }
}

// The peer's matrix and vector of each value type.
template <typename T> struct Peer;
template <> struct Peer<f64>
{
    using Matrix = peer::SpF64;
    using Vector = peer::VecF64;
};
template <> struct Peer<f32>
{
    using Matrix = peer::SpF32;
    using Vector = peer::VecF32;
};
template <> struct Peer<Complex<f64>>
{
    using Matrix = peer::SpC64;
    using Vector = peer::VecC64;
};
template <> struct Peer<Complex<f32>>
{
    using Matrix = peer::SpC32;
    using Vector = peer::VecC32;
};

struct Coord
{
    u32 r;
    u32 c;
    f64 re;
    f64 im; // 0 for a real matrix
};

enum class Pattern : crd::u8
{
    Uniform,
    Banded,
    PowerLaw,
};

struct Shape
{
    const char* name;
    Pattern pattern;
    u32 per_row; // the stored values of a row: exact for uniform, the band for banded
    f64 mean;    // the mean stored values of a row, which sizes the matrix
};

constexpr Shape kShapes[] = {
    {"uniform-16", Pattern::Uniform, 16U, 16.0},
    {"uniform-4", Pattern::Uniform, 4U, 4.0},
    {"banded-5", Pattern::Banded, 5U, 5.0},
    {"power-law", Pattern::PowerLaw, 0U, 8.2}, // nine rows in ten store 2, the tenth 64
};

crd::containers::Array<Coord> generate(const Shape& shape, u32 n, bool complex_values, u64 seed)
{
    crd::containers::Array<Coord> out(&g_alloc);
    u64 s = seed;
    auto next = [&]()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(s >> 33);
    };
    auto push = [&](u32 r, u32 c)
    {
        const f64 re = 1.0 + static_cast<f64>(next() % 7U);
        const f64 im = complex_values ? static_cast<f64>(next() % 5U) - 2.0 : 0.0;
        out.push_back(Coord{r, c, re, im});
    };
    out.reserve(static_cast<usize>(static_cast<f64>(n) * shape.mean) + 64U);
    for (u32 r = 0; r < n; ++r)
    {
        if (shape.pattern == Pattern::Uniform)
        {
            for (u32 k = 0; k < shape.per_row; ++k)
            {
                push(r, next() % n);
            }
        }
        else if (shape.pattern == Pattern::Banded)
        {
            const crd::i64 half = shape.per_row / 2U;
            for (crd::i64 c = static_cast<crd::i64>(r) - half; c <= static_cast<crd::i64>(r) + half; ++c)
            {
                if (c >= 0 && c < static_cast<crd::i64>(n))
                {
                    push(r, static_cast<u32>(c));
                }
            }
        }
        else
        {
            const u32 degree = next() % 10U == 0U ? 64U : 2U;
            for (u32 k = 0; k < degree; ++k)
            {
                push(r, next() % n);
            }
        }
    }
    return out;
}

template <typename T> typename Peer<T>::Matrix* peer_matrix(u32 n, const crd::containers::Array<Coord>& coords)
{
    const auto count = static_cast<peer::Index>(coords.size());
    if constexpr (kComplex<T>)
    {
        crd::containers::Array<peer::ComplexEntry> entries(&g_alloc);
        entries.reserve(coords.size());
        for (const Coord& e : coords)
        {
            entries.push_back(peer::ComplexEntry{static_cast<crd::i32>(e.r), static_cast<crd::i32>(e.c), e.re, e.im});
        }
        if constexpr (std::is_same_v<Part<T>, f64>)
        {
            return peer::csr_c64(n, n, entries.data(), count);
        }
        else
        {
            return peer::csr_c32(n, n, entries.data(), count);
        }
    }
    else
    {
        crd::containers::Array<peer::Entry> entries(&g_alloc);
        entries.reserve(coords.size());
        for (const Coord& e : coords)
        {
            entries.push_back(peer::Entry{static_cast<crd::i32>(e.r), static_cast<crd::i32>(e.c), e.re});
        }
        if constexpr (std::is_same_v<T, f64>)
        {
            return peer::csr_f64(n, n, entries.data(), count);
        }
        else
        {
            return peer::csr_f32(n, n, entries.data(), count);
        }
    }
}

template <typename T> typename Peer<T>::Vector* peer_vector(u32 n, const T* values)
{
    if constexpr (std::is_same_v<T, Complex<f64>>)
    {
        return peer::vec_c64(n, reinterpret_cast<const f64*>(values));
    }
    else if constexpr (std::is_same_v<T, Complex<f32>>)
    {
        return peer::vec_c32(n, reinterpret_cast<const f32*>(values));
    }
    else
    {
        return peer::vec(n, values);
    }
}

template <typename Fn> f64 time_ms(int inner, const Fn& fn)
{
    const auto t0 = Clock::now();
    for (int k = 0; k < inner; ++k)
    {
        fn();
    }
    const auto t1 = Clock::now();
    return std::chrono::duration<f64, std::milli>(t1 - t0).count() / inner;
}

const char* gather_name(sp::SellGather gather)
{
    return gather == sp::SellGather::Loads ? "loads" : "gather";
}

template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csr> assemble(u32 n, const crd::containers::Array<Coord>& coords)
{
    sp::TripletBuilder<T> b(&g_alloc, n, n);
    b.reserve(coords.size());
    for (const Coord& e : coords)
    {
        b.add(e.r, e.c, scalar<T>(e.re, e.im));
    }
    return b.compress();
}

template <typename T> crd::containers::Array<T> x_values(u32 n)
{
    crd::containers::Array<T> x(&g_alloc);
    x.resize(n);
    for (u32 i = 0; i < n; ++i)
    {
        x[i] = scalar<T>(1.0 + static_cast<f64>(i % 13U) * 0.01, 0.5 - static_cast<f64>(i % 7U) * 0.01);
    }
    return x;
}

// 4 (len + 2) eps sum |a| |x| a row: twice the worst error of either side's in-order sum, with room for a complex
// product's.
template <typename T>
crd::containers::Array<f64> eigen_bounds(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a,
                                         const crd::containers::Array<T>& x)
{
    const sp::SparsePattern& pat = a.pattern();
    const T* values = a.values().values.data();
    const f64 eps = std::numeric_limits<Part<T>>::epsilon();
    crd::containers::Array<f64> bound(&g_alloc);
    bound.resize(pat.rows);
    for (u32 r = 0; r < pat.rows; ++r)
    {
        f64 sum = 0.0;
        for (u32 k = pat.outer_ptr[r]; k < pat.outer_ptr[r + 1U]; ++k)
        {
            sum += magnitude(values[k]) * magnitude(x[pat.inner_idx[k]]);
        }
        bound[r] = 4.0 * static_cast<f64>(pat.outer_ptr[r + 1U] - pat.outer_ptr[r] + 2U) * eps * sum;
    }
    return bound;
}

// One matrix: the CSR reference and its y, its Auto SELL build and the column form Auto did not pick (a matrix Auto
// built as Delta16 without one Delta16 slice counts as U32, whose bytes it has), the peer's copy, x, and each row's
// Eigen bound. Built in place from coordinates that die with the constructor's full expression.
template <typename T> struct Problem
{
    Problem(const char* shape_name, const char* band_name, u32 rows, const crd::containers::Array<Coord>& coords)
        : shape(shape_name), band(band_name), n(rows), csr(assemble<T>(rows, coords)),
          automatic(sp::to_sell(csr, &g_alloc)),
          other(sp::to_sell(csr, &g_alloc, 0U, auto_is_u32() ? sp::SellIndex::Delta16 : sp::SellIndex::U32)),
          x(x_values<T>(rows)), y(&g_alloc), reference(&g_alloc), bound(eigen_bounds(csr, x)),
          peer_a(peer_matrix<T>(rows, coords)), peer_x(peer_vector<T>(rows, x.data())),
          peer_y(peer_vector<T>(rows, static_cast<const T*>(nullptr)))
    {
        y.resize(n);
        reference.resize(n);
        sp::spmv<T>(scalar<T>(1.0, 0.0), csr, sp::Trans::None, {x.data(), x.size()}, T{},
                    {reference.data(), reference.size()});
        // About 4M products a timing at least, so an L2-resident matrix runs far above the clock's resolution.
        const usize stored = csr.pattern().outer_ptr[n];
        inner = static_cast<int>(std::clamp<usize>((usize{1} << 22U) / std::max<usize>(stored, 1U), 1U, 256U));
    }

    [[nodiscard]] bool auto_is_u32() const { return automatic.col_first.empty(); }
    [[nodiscard]] const sp::SellMatrix<T>& u32_form() const { return auto_is_u32() ? automatic : other; }
    [[nodiscard]] const sp::SellMatrix<T>& delta16() const { return auto_is_u32() ? other : automatic; }

    const char* shape;
    const char* band;
    u32 n;
    sp::SparseMatrix<T, sp::SparseFormat::Csr> csr;
    sp::SellMatrix<T> automatic;
    sp::SellMatrix<T> other;
    crd::containers::Array<T> x;
    crd::containers::Array<T> y;
    crd::containers::Array<T> reference;
    crd::containers::Array<f64> bound;
    crd_peer::Owned<typename Peer<T>::Matrix> peer_a;
    crd_peer::Owned<typename Peer<T>::Vector> peer_x;
    crd_peer::Owned<typename Peer<T>::Vector> peer_y;
    int inner = 1;
};

// y poisoned before an arm's checked run, so a row it does not write cannot pass.
template <typename T> void poison(Problem<T>& p)
{
    std::memset(static_cast<void*>(p.y.data()), 0xFF, p.y.size() * sizeof(T));
}

template <typename T> bool same_bits(const Problem<T>& p, const char* arm)
{
    if (std::memcmp(p.y.data(), p.reference.data(), p.y.size() * sizeof(T)) == 0)
    {
        return true;
    }
    std::fprintf(stderr, "FAIL: %s %s n=%u, arm %s: bits differ from the CSR spmv\n", type_name<T>(), p.shape, p.n,
                 arm);
    g_failed = true;
    return false;
}

template <typename T> void check_peer(const Problem<T>& p, const char* arm)
{
    const T* y = reinterpret_cast<const T*>(peer::data(p.peer_y.get()));
    for (u32 r = 0; r < p.n; ++r)
    {
        const f64 err = distance(y[r], p.reference[r]);
        if (!(err <= p.bound[r]))
        {
            std::fprintf(stderr, "FAIL: %s %s n=%u, %s: row %u off by %.3g, bound %.3g\n", type_name<T>(), p.shape, p.n,
                         arm, r, err, p.bound[r]);
            g_failed = true;
            return;
        }
    }
}

template <typename T> void print_case(const Problem<T>& p, const char* phase)
{
    const usize stored = p.csr.pattern().outer_ptr[p.n];
    std::printf("\n==== %s %s %s, %u rows (%s), %.1f stored a row, %.1f MiB SELL, Auto index %s ====\n", phase,
                type_name<T>(), p.shape, p.n, p.band, static_cast<f64>(stored) / p.n,
                static_cast<f64>(p.automatic.stored_bytes()) / (1024.0 * 1024.0), p.auto_is_u32() ? "U32" : "Delta16");
}

// The serial arms: a SellPick down sell_spmv_range, or the production spmv_sell.
template <typename T> struct SerialArm
{
    char name[32];
    const sp::SellMatrix<T>* a;
    bool production;
    usize resident;
    sp::SellGather gather;
    u32 pair_width;
};

constexpr u32 kPairWidths[] = {1U, 8U, 16U, 32U, sp::kSellNoPairs};

template <typename T> void serial_board(Problem<T>& p)
{
    static constexpr usize kMaxArms = 5U + 2U * (sizeof(kPairWidths) / sizeof(kPairWidths[0]));
    SerialArm<T> arms[kMaxArms]{};
    usize count = 0;
    const usize l2 = sp::sparse_resident_bytes();
    auto add = [&](const char* name, const sp::SellMatrix<T>& a, bool production, usize resident, sp::SellGather gather,
                   u32 pair_width)
    {
        SerialArm<T>& arm = arms[count++];
        (void)std::snprintf(arm.name, sizeof(arm.name), "%s", name);
        arm.a = &a;
        arm.production = production;
        arm.resident = resident;
        arm.gather = gather;
        arm.pair_width = pair_width;
    };
    add("spmv_sell", p.automatic, true, l2, sp::SellGather::Policy, 0U);
    add("index u32", p.u32_form(), false, l2, sp::SellGather::Policy, 0U);
    add("index delta16", p.delta16(), false, l2, sp::SellGather::Policy, 0U);
    add("policy resident", p.automatic, false, ~usize{0}, sp::SellGather::Policy, 0U);
    add("policy streaming", p.automatic, false, 0U, sp::SellGather::Policy, 0U);
    for (const sp::SellGather gather : {sp::SellGather::Loads, sp::SellGather::Gather})
    {
        for (const u32 width : kPairWidths)
        {
            char name[32];
            if (width == sp::kSellNoPairs)
            {
                (void)std::snprintf(name, sizeof(name), "%s, no pairs", gather_name(gather));
            }
            else
            {
                (void)std::snprintf(name, sizeof(name), "%s, pairs %u", gather_name(gather), width);
            }
            add(name, p.automatic, false, l2, gather, width);
        }
    }

    const T one = scalar<T>(1.0, 0.0);
    auto run = [&](const SerialArm<T>& arm)
    {
        if (arm.production)
        {
            sp::spmv_sell<T>(one, *arm.a, {p.x.data(), p.x.size()}, T{}, {p.y.data(), p.y.size()});
            return true;
        }
        return sp::detail::sell_spmv_range<T>(*arm.a, one, T{}, p.x.data(), p.y.data(), 0U, arm.a->num_slices,
                                              arm.resident, arm.gather, arm.pair_width);
    };

    // A gather is refused exactly where the level has no gather form: portable and sse2.
    const crd::CpuLevel level = sp::sparse_kernels().level;
    const bool level_gathers = level == crd::CpuLevel::Avx2 || level == crd::CpuLevel::Avx512;
    bool live[kMaxArms]{};
    f64 best[kMaxArms]{};
    for (usize i = 0; i < count; ++i)
    {
        poison(p);
        live[i] = run(arms[i]);
        best[i] = 1e300;
        const bool refusable = arms[i].gather == sp::SellGather::Gather && !level_gathers;
        if (!live[i] && !refusable)
        {
            std::fprintf(stderr, "FAIL: %s %s n=%u, arm %s: refused at %s\n", type_name<T>(), p.shape, p.n,
                         arms[i].name, crd::cpu_level_name(level));
            g_failed = true;
        }
        else if (live[i] && refusable)
        {
            std::fprintf(stderr, "FAIL: %s %s n=%u, arm %s: ran at %s, which has no gather\n", type_name<T>(), p.shape,
                         p.n, arms[i].name, crd::cpu_level_name(level));
            g_failed = true;
        }
        if (live[i])
        {
            live[i] = same_bits(p, arms[i].name);
        }
    }
    peer::set_threads(1);
    peer::spmv(p.peer_a.get(), p.peer_x.get(), p.peer_y.get());
    check_peer(p, "Eigen, one thread");

    f64 eigen = 1e300;
    for (int round = 0; round < kRounds; ++round)
    {
        for (usize i = 0; i < count; ++i)
        {
            if (live[i])
            {
                best[i] = std::min(best[i], time_ms(p.inner, [&]() { (void)run(arms[i]); }));
            }
        }
        eigen =
            std::min(eigen, time_ms(p.inner, [&]() { peer::spmv(p.peer_a.get(), p.peer_x.get(), p.peer_y.get()); }));
    }

    print_case(p, "one thread");
    std::printf("%-18s | %10s | %9s | %9s\n", "arm", "ms", "x Eigen", "x spmv");
    for (usize i = 0; i < count; ++i)
    {
        if (!live[i])
        {
            std::printf("%-18s | %10s | %9s | %9s\n", arms[i].name, "n/a", "n/a", "n/a");
            continue;
        }
        std::printf("%-18s | %10.4f | %8.3fx | %8.3fx\n", arms[i].name, best[i], eigen / best[i], best[0] / best[i]);
    }
    std::printf("%-18s | %10.4f |\n", "Eigen", eigen);
}

// The job-cut arms of one job count: each column form at each cut.
struct ParallelArm
{
    const char* name;
    bool delta16;
    sp::SellJobCut cut;
};

constexpr ParallelArm kParallelArms[] = {
    {"u32 whole-L2", false, sp::SellJobCut::WholeL2},
    {"u32 share", false, sp::SellJobCut::CoreShare},
    {"delta16 whole-L2", true, sp::SellJobCut::WholeL2},
    {"delta16 share", true, sp::SellJobCut::CoreShare},
};
constexpr usize kParallelArmCount = sizeof(kParallelArms) / sizeof(kParallelArms[0]);

template <typename T> void parallel_board(Problem<T>& p, u32 cpus)
{
    const T one = scalar<T>(1.0, 0.0);
    auto run = [&](usize jobs_index, usize arm)
    {
        const sp::SellMatrix<T>& a = kParallelArms[arm].delta16 ? p.delta16() : p.u32_form();
        sp::detail::spmv_sell_parallel<T>(one, a, {p.x.data(), p.x.size()}, T{}, {p.y.data(), p.y.size()},
                                          kJobs[jobs_index], kParallelArms[arm].cut);
    };
    bool live[kJobCounts][kParallelArmCount]{};
    f64 best[kJobCounts][kParallelArmCount]{};
    f64 eigen[kJobCounts]{};
    for (usize j = 0; j < kJobCounts; ++j)
    {
        eigen[j] = 1e300;
        for (usize i = 0; i < kParallelArmCount; ++i)
        {
            best[j][i] = 1e300;
            if (kJobs[j] <= cpus)
            {
                poison(p);
                run(j, i);
                live[j][i] = same_bits(p, kParallelArms[i].name);
            }
        }
    }
    for (int round = 0; round < kRounds; ++round)
    {
        for (usize j = 0; j < kJobCounts; ++j)
        {
            for (usize i = 0; i < kParallelArmCount; ++i)
            {
                if (live[j][i])
                {
                    best[j][i] = std::min(best[j][i], time_ms(p.inner, [&]() { run(j, i); }));
                }
            }
        }
    }
    for (usize j = 0; j < kJobCounts && kJobs[j] <= cpus; ++j)
    {
        peer::set_threads(static_cast<int>(kJobs[j]));
        peer::spmv(p.peer_a.get(), p.peer_x.get(), p.peer_y.get());
        check_peer(p, "Eigen, threaded");
        for (int round = 0; round < kRounds; ++round)
        {
            eigen[j] = std::min(
                eigen[j], time_ms(p.inner, [&]() { peer::spmv(p.peer_a.get(), p.peer_x.get(), p.peer_y.get()); }));
        }
    }
    peer::set_threads(1);

    print_case(p, "threads");
    std::printf("%-5s |", "jobs");
    for (const ParallelArm& arm : kParallelArms)
    {
        std::printf(" %16s |", arm.name);
    }
    std::printf(" %10s | %s\n", "Eigen", "best x Eigen");
    for (usize j = 0; j < kJobCounts; ++j)
    {
        std::printf("%-5u |", kJobs[j]);
        if (kJobs[j] > cpus)
        {
            std::printf(" n/a: the process may run on %u CPUs\n", cpus);
            continue;
        }
        f64 fastest = 1e300;
        for (usize i = 0; i < kParallelArmCount; ++i)
        {
            if (live[j][i])
            {
                fastest = std::min(fastest, best[j][i]);
                std::printf(" %16.4f |", best[j][i]);
            }
            else
            {
                std::printf(" %16s |", "failed");
            }
        }
        std::printf(" %10.4f | %8.3fx\n", eigen[j], eigen[j] / fastest);
    }
}

struct Band
{
    const char* name;
    usize bytes; // the matrix, x and y; 0 for a fixed row count
    u32 rows;
};

// Rows of `shape` whose SELL values, columns, row lengths, x and y fill `bytes`.
template <typename T> u32 rows_for(const Shape& shape, usize bytes)
{
    const f64 per_row = shape.mean * static_cast<f64>(sizeof(T) + sizeof(u32)) + sizeof(u32) + 2.0 * sizeof(T);
    return std::max(64U, static_cast<u32>(static_cast<f64>(bytes) / per_row));
}

template <typename T> void run_type(const Band* bands, usize band_count, bool one_thread, bool threads, u32 cpus)
{
    for (usize b = 0; b < band_count; ++b)
    {
        for (const Shape& shape : kShapes)
        {
            const u32 n = bands[b].bytes != 0U ? rows_for<T>(shape, bands[b].bytes) : bands[b].rows;
            Problem<T> p(shape.name, bands[b].name, n,
                         generate(shape, n, kComplex<T>, 0xBEEFULL ^ n ^ (static_cast<u64>(shape.per_row) << 20U)));
            if (one_thread)
            {
                serial_board(p);
            }
            if (threads)
            {
                parallel_board(p, cpus);
            }
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    bool one_thread = argc == 1;
    bool threads = argc == 1;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--one-thread") == 0)
        {
            one_thread = true;
        }
        else if (std::strcmp(argv[i], "--threads") == 0)
        {
            threads = true;
        }
        else
        {
            std::fprintf(stderr, "usage: bench_hesap_spmv_vs_reference [--one-thread] [--threads]\n");
            return 2;
        }
    }
    if (threads)
    {
        crd::jobs::init();
    }

    const crd::CpuSelection& cpu = crd::cpu_selection();
    const usize l2 = sp::sparse_resident_bytes();
    const usize l3_reported = cpu.features.caches.l3_bytes;
    const usize l3 = l3_reported > l2 ? l3_reported : 16U * l2;
    const u32 cpus = cpu.cpus_visited;
    std::printf("[bench_hesap_spmv] y = A x, Cerid SELL-C-sigma against Eigen, best of %d rounds, ms. hesap-sparse "
                "level %s (selected %s); %s\n",
                kRounds, crd::cpu_level_name(sp::sparse_kernels().level), crd::cpu_level_name(cpu.selected),
                peer::isa());
    std::printf("L2 %zu KiB (a job's share %zu KiB), L3 %zu KiB%s, %u CPUs\n", l2 / 1024U,
                sp::sparse_job_resident_bytes() / 1024U, l3 / 1024U,
                l3_reported > l2 ? "" : " (none reported: 16 times the L2)", cpus);

    const Band bands[] = {
        {"half the L2", l2 / 2U, 0U},
        {"between the L2 and the L3", static_cast<usize>(std::sqrt(static_cast<f64>(l2) * static_cast<f64>(l3))), 0U},
        {"half the L3", l3 / 2U, 0U},
        {"DRAM, 1M rows", 0U, 1000000U},
        {"DRAM, 2M rows", 0U, 2000000U},
    };
    static constexpr usize kBands = sizeof(bands) / sizeof(bands[0]);
    run_type<f64>(bands, kBands, one_thread, threads, cpus);
    run_type<f32>(bands, kBands, one_thread, threads, cpus);
    run_type<Complex<f64>>(bands, kBands, one_thread, threads, cpus);
    run_type<Complex<f32>>(bands, kBands, one_thread, threads, cpus);

    if (threads)
    {
        crd::jobs::shutdown();
    }
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_spmv: a gate failed\n");
        return 1;
    }
    return 0;
}
