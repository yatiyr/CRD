// bench_permute — the permute board against HPTT (SIMD-DISPATCH; scripts/run_permute_bench.sh, at each CPU level).
// dst = src.permute(order) of f32, f64, Complex<f32> and Complex<f64> over the HPTT oracle harness's three cases
// (external/PEER_ORACLES.md; its column-major cases in row-major order: {1,0} and the full reversal are their own
// mirror, col-major 512^3 {2,0,1} is row-major {1,2,0}), each arm a detail::PermuteArm of permute_copy (permute.hpp),
// timed round by round in turn so the arms share the host's state, the median of the rounds reported.
//
// --one-thread: the serial walk's arms: the rules, tile edges 32 and 64, non-temporal stores, HPTT's outer order, the
// prefetch hints T0, T1 and NTA, prefetch distances 0 and 2, no destination prefetch, and the scalar tile in place of
// the level's register transposes. --threads N: the multithreaded tasks' arms on N workers: the rules, stores forced
// regular or non-temporal, staged full-column strips (each also with forced stores), tiles and super-blocks for 8- and
// 16-byte elements, HPTT's order, distance 2, the T0 hint and the scalar tile. No argument runs --one-thread.
//
// Every arm must give the bits of a reference walk of src.permute(order) (a plain copy, the family is bit-exact on
// every level and worker count, ADR-0063); a refusal or a miss fails the run. GB/s counts one read and one write per
// element, the oracle table's basis. HPTT itself is the harness in external/hptt (f32, a local clone).
//
// crd::containers types and one GrowableTlsfAllocator (the 3D case's three 2 GiB tensors), never std containers or
// malloc.

#include <crd/containers/span.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/tensor/permute.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace
{
namespace tn = crd::hesap::tensor;
using crd::f32;
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::hesap::Complex;
using tn::PermuteHint;
using tn::PermuteOrder;
using tn::PermutePick;
using tn::detail::PermuteArm;
using tn::detail::PermuteStream;
using Clock = std::chrono::steady_clock;

crd::memory::GrowableTlsfAllocator g_alloc;
bool g_failed = false;

constexpr int kRounds = 10;
constexpr usize kMaxArms = 20U;

struct Case
{
    const char* name;
    u32 rank;
    u64 shape[4];
    u32 order[4];
};

constexpr Case kCases[] = {
    {"2D 4096^2 {1,0}", 2U, {4096U, 4096U, 0U, 0U}, {1U, 0U, 0U, 0U}},
    {"4D 64^4 {3,2,1,0}", 4U, {64U, 64U, 64U, 64U}, {3U, 2U, 1U, 0U}},
    {"3D 512^3 {1,2,0}", 3U, {512U, 512U, 512U, 0U}, {1U, 2U, 0U, 0U}},
};

struct Arm
{
    const char* name;
    PermuteArm arm;
};

constexpr PermutePick pick_hint(PermuteHint hint)
{
    PermutePick p{};
    p.hint = hint;
    return p;
}

constexpr PermutePick pick_distance(u32 distance)
{
    PermutePick p{};
    p.distance = static_cast<crd::u8>(distance);
    return p;
}

constexpr PermutePick kHpttOrder{PermuteHint::T2, 1U, true, false, PermuteOrder::Hptt};
constexpr PermutePick kNoDstPrefetch{PermuteHint::T2, 1U, false, false, PermuteOrder::Src};
constexpr PermutePick kScalar{PermuteHint::T2, 1U, true, true, PermuteOrder::Src};

constexpr PermuteArm serial(PermutePick pick, u64 tile, PermuteStream stream)
{
    return PermuteArm{pick, tile, 0U, 0U, 0U, 0U, stream, false};
}

constexpr PermuteArm parallel(PermutePick pick, u64 tile_a, u64 tile_b, u64 sup_a, u64 sup_b, PermuteStream stream,
                              bool staged)
{
    return PermuteArm{pick, 0U, tile_a, tile_b, sup_a, sup_b, stream, staged};
}

constexpr PermuteStream kAuto = PermuteStream::Auto;

constexpr Arm kSerialArms[] = {
    {"rules", {}},
    {"tile 32", serial({}, 32U, kAuto)},
    {"tile 64", serial({}, 64U, kAuto)},
    {"stream", serial({}, 0U, PermuteStream::On)},
    {"order hptt", serial(kHpttOrder, 0U, kAuto)},
    {"hint t0", serial(pick_hint(PermuteHint::T0), 0U, kAuto)},
    {"hint t1", serial(pick_hint(PermuteHint::T1), 0U, kAuto)},
    {"hint nta", serial(pick_hint(PermuteHint::Nta), 0U, kAuto)},
    {"distance 0", serial(pick_distance(0U), 0U, kAuto)},
    {"distance 2", serial(pick_distance(2U), 0U, kAuto)},
    {"no dst prefetch", serial(kNoDstPrefetch, 0U, kAuto)},
    {"scalar tile", serial(kScalar, 0U, kAuto)},
};

constexpr Arm kParallelArms[] = {
    {"rules", {}},
    {"stream off", parallel({}, 0U, 0U, 0U, 0U, PermuteStream::Off, false)},
    {"stream on", parallel({}, 0U, 0U, 0U, 0U, PermuteStream::On, false)},
    {"staged", parallel({}, 0U, 0U, 0U, 0U, kAuto, true)},
    {"staged, stream off", parallel({}, 0U, 0U, 0U, 0U, PermuteStream::Off, true)},
    {"staged, stream on", parallel({}, 0U, 0U, 0U, 0U, PermuteStream::On, true)},
    {"tiles 16x16", parallel({}, 16U, 16U, 0U, 0U, kAuto, false)},
    {"tiles 32x16", parallel({}, 32U, 16U, 0U, 0U, kAuto, false)},
    {"tiles 64x64", parallel({}, 64U, 64U, 0U, 0U, kAuto, false)},
    {"tall 256x16", parallel({}, 256U, 16U, 2U, 8U, kAuto, false)},
    {"tall 128x8", parallel({}, 128U, 8U, 2U, 8U, kAuto, false)},
    {"supers 4x4", parallel({}, 0U, 0U, 4U, 4U, kAuto, false)},
    {"supers 16x16", parallel({}, 0U, 0U, 16U, 16U, kAuto, false)},
    {"order hptt", parallel(kHpttOrder, 0U, 0U, 0U, 0U, kAuto, false)},
    {"distance 2", parallel(pick_distance(2U), 0U, 0U, 0U, 0U, kAuto, false)},
    {"hint t0", parallel(pick_hint(PermuteHint::T0), 0U, 0U, 0U, 0U, kAuto, false)},
    {"scalar tile", parallel(kScalar, 0U, 0U, 0U, 0U, kAuto, false)},
};

static_assert(sizeof(kSerialArms) / sizeof(Arm) <= kMaxArms && sizeof(kParallelArms) / sizeof(Arm) <= kMaxArms);

template <typename T> const char* type_name()
{
    if constexpr (std::is_same_v<T, f32>)
    {
        return "f32";
    }
    else if constexpr (std::is_same_v<T, f64>)
    {
        return "f64";
    }
    else if constexpr (std::is_same_v<T, Complex<f32>>)
    {
        return "c32";
    }
    else
    {
        return "c64";
    }
}

template <typename T> T value(u64 i)
{
    if constexpr (std::is_same_v<T, f32> || std::is_same_v<T, f64>)
    {
        return static_cast<T>(i & 0xFFFFU) * static_cast<T>(0.001);
    }
    else
    {
        using S = decltype(T{}.re);
        return T{static_cast<S>(i & 0xFFFFU) * static_cast<S>(0.001), static_cast<S>(i >> 16U)};
    }
}

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return n % 2 == 1 ? times[n / 2] : 0.5 * (times[n / 2 - 1] + times[n / 2]);
}

// One case's arms: each gated once against the reference walk (dst poisoned first, so an element no arm writes
// cannot pass), then timed round by round in turn.
template <typename T> void board(const Case& c, const Arm* arms, usize count, const char* phase)
{
    const crd::containers::ConstSpan<u64> shape{c.shape, c.rank};
    const crd::containers::ConstSpan<u32> order{c.order, c.rank};
    tn::Tensor<T> src(&g_alloc, shape);
    for (u64 i = 0; i < src.size(); ++i)
    {
        src.data()[i] = value<T>(i);
    }
    const tn::TensorView<const T> view(src.view());
    u64 oshape[tn::kMaxRank] = {};
    crd::i64 istride[tn::kMaxRank] = {};
    for (u32 d = 0; d < c.rank; ++d)
    {
        oshape[d] = src.shape(c.order[d]);
        istride[d] = view.stride(c.order[d]);
    }
    const crd::containers::ConstSpan<u64> dst_shape{oshape, c.rank};
    tn::Tensor<T> reference(&g_alloc, dst_shape);
    u64 k = 0;
    view.permute(order).for_each([&](const u64* /*idx*/, const T& v) { reference.data()[k++] = v; });
    tn::Tensor<T> dst(&g_alloc, dst_shape);
    const usize bytes = static_cast<usize>(dst.size()) * sizeof(T);
    const auto run = [&](const PermuteArm& arm)
    {
        return tn::detail::permute_impl<false>(dst.data(), src.data(), oshape, istride, c.rank, T{1}, arm);
    };

    bool live[kMaxArms] = {};
    for (usize a = 0; a < count; ++a)
    {
        std::memset(static_cast<void*>(dst.data()), 0xFF, bytes);
        const tn::TensorStatus st = run(arms[a].arm);
        live[a] = st == tn::TensorStatus::Ok && std::memcmp(dst.data(), reference.data(), bytes) == 0;
        if (!live[a])
        {
            std::fprintf(stderr, "FAIL: %s %s %s, arm %s: %s\n", phase, type_name<T>(), c.name, arms[a].name,
                         st == tn::TensorStatus::Ok ? "bits differ from the reference walk" : "refused");
            g_failed = true;
        }
    }
    f64 times[kMaxArms][kRounds] = {};
    for (int round = 0; round < kRounds; ++round)
    {
        for (usize a = 0; a < count; ++a)
        {
            if (live[a])
            {
                const auto t0 = Clock::now();
                (void)run(arms[a].arm);
                const auto t1 = Clock::now();
                times[a][round] = std::chrono::duration<f64, std::nano>(t1 - t0).count();
            }
        }
    }

    const f64 elements = static_cast<f64>(dst.size());
    std::printf("\n==== %s %s %s, %.1f MiB a tensor ====\n", phase, type_name<T>(), c.name,
                static_cast<f64>(bytes) / (1024.0 * 1024.0));
    std::printf("%-20s | %10s | %8s | %s\n", "arm", "ns/elem", "GB/s", "x rules");
    const f64 rules = live[0] ? median(times[0], kRounds) : 0.0;
    for (usize a = 0; a < count; ++a)
    {
        if (!live[a])
        {
            std::printf("%-20s | failed\n", arms[a].name);
            continue;
        }
        const f64 ns = a == 0 ? rules : median(times[a], kRounds);
        std::printf("%-20s | %10.4f | %8.2f | %7.3fx\n", arms[a].name, ns / elements,
                    2.0 * static_cast<f64>(bytes) / ns, rules > 0.0 ? rules / ns : 0.0);
    }
}

template <typename T> void run_type(const Arm* arms, usize count, const char* phase)
{
    for (const Case& c : kCases)
    {
        board<T>(c, arms, count, phase);
    }
}

template <usize N> void run_all(const Arm (&arms)[N], const char* phase)
{
    run_type<f32>(arms, N, phase);
    run_type<f64>(arms, N, phase);
    run_type<Complex<f32>>(arms, N, phase);
    run_type<Complex<f64>>(arms, N, phase);
}
} // namespace

int main(int argc, char** argv)
{
    u32 workers = 0;
    if (argc == 3 && std::strcmp(argv[1], "--threads") == 0)
    {
        workers = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    }
    else if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--one-thread") != 0))
    {
        std::fprintf(stderr, "usage: bench_permute [--one-thread | --threads N]\n");
        return 2;
    }
    if (argc == 3 && workers < 2U)
    {
        std::fprintf(stderr, "bench_permute: --threads needs 2 or more workers\n");
        return 2;
    }
    const crd::CpuSelection& cpu = crd::cpu_selection();
    std::printf("[bench_permute] dst = src.permute(order), median of %d rounds; hesap-tensor level %s (selected %s), "
                "%s\n",
                kRounds, crd::cpu_level_name(tn::tensor_kernels().level), crd::cpu_level_name(cpu.selected),
                workers == 0U ? "one thread" : "multithreaded");
    if (workers == 0U)
    {
        run_all(kSerialArms, "one thread");
    }
    else
    {
        crd::jobs::Config cfg;
        cfg.num_threads = workers;
        crd::jobs::init(cfg);
        std::printf("%u workers\n", crd::jobs::num_workers());
        run_all(kParallelArms, "threads");
        crd::jobs::shutdown();
    }
    if (g_failed)
    {
        std::fprintf(stderr, "bench_permute: a gate failed\n");
        return 1;
    }
    return 0;
}
