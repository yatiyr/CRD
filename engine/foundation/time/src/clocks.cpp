// crd-time -- clock implementations (Detour D-006).

#include <crd/time/clocks.hpp>

#include <crd/core/platform.hpp> // CRD_NOINLINE

#include <atomic>
#include <chrono>

#if defined(_MSC_VER)
    #include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    #include <cpuid.h>
#endif

namespace crd::time
{

// ===========================================================================
// MonotonicClock
// ===========================================================================

Instant MonotonicClock::now() noexcept
{
    const auto tp = std::chrono::steady_clock::now();
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return Instant{static_cast<crd::i64>(since_epoch)};
}

// ===========================================================================
// WallClock
// ===========================================================================

Instant WallClock::now() noexcept
{
    const auto tp = std::chrono::system_clock::now();
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return Instant{static_cast<crd::i64>(since_epoch)};
}

// ===========================================================================
// CycleCounter
// ===========================================================================

crd::u64 CycleCounter::now() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<crd::u64>(hi) << 32) | static_cast<crd::u64>(lo);
#elif defined(__aarch64__)
    crd::u64 val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Fallback: derive a "cycle"-ish counter from the monotonic clock's
    // nanoseconds. Not great but at least monotonic.
    return static_cast<crd::u64>(MonotonicClock::now().ns_since_epoch());
#endif
}

crd::f64 CycleCounter::calibrate(Duration calibration_window) noexcept
{
    // Spin-loop calibration: measure cycles per unit of wall-clock time.
    const Instant wall_start = MonotonicClock::now();
    const crd::u64 cyc_start = CycleCounter::now();
    const Instant wall_end_target = wall_start + calibration_window;

    while (MonotonicClock::now() < wall_end_target)
    {
        // Busy-wait. The calibration window should be small (default 10 ms).
    }

    const crd::u64 cyc_end = CycleCounter::now();
    const Instant wall_end = MonotonicClock::now();

    const Duration elapsed = wall_end - wall_start;
    const crd::u64 cycles = cyc_end - cyc_start;

    if (elapsed.value <= 0.0)
    {
        return 0.0;  // degenerate (should not happen with sane inputs)
    }
    return static_cast<crd::f64>(cycles) / elapsed.value;
}

// ===========================================================================
// TickClock
// ===========================================================================

namespace
{

// The counter where TickClock may map it: x86-64's TSC when CPUID reports it invariant (constant rate in every
// P-, C- and T-state), ARM64's generic timer (a constant frequency by architecture); nowhere else.
bool counter_invariant() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int regs[4] = {}; // NOLINT(modernize-avoid-c-arrays): __cpuid's out-parameter
    __cpuid(regs, static_cast<int>(0x80000000U));
    if (static_cast<unsigned>(regs[0]) < 0x80000007U)
    {
        return false;
    }
    __cpuid(regs, static_cast<int>(0x80000007U));
    return (static_cast<unsigned>(regs[3]) & (1U << 8U)) != 0U;
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    unsigned eax = 0U;
    unsigned ebx = 0U;
    unsigned ecx = 0U;
    unsigned edx = 0U;
    if (__get_cpuid(0x80000000U, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000007U)
    {
        return false;
    }
    if (__get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx) == 0)
    {
        return false;
    }
    return (edx & (1U << 8U)) != 0U;
#elif defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

// (ticks * ns_per_tick_q32) >> 32: a tick count scaled by a 32.32 fixed-point rate, without overflow for any count.
crd::i64 scale(crd::u64 ticks, crd::u64 rate_q32) noexcept
{
#if defined(__SIZEOF_INT128__)
    __extension__ using u128 = unsigned __int128; // __extension__ keeps it under -Werror=pedantic
    return static_cast<crd::i64>((static_cast<u128>(ticks) * static_cast<u128>(rate_q32)) >> 32U);
#elif defined(_MSC_VER) && defined(_M_X64)
    crd::u64 high = 0U;
    const crd::u64 low = _umul128(ticks, rate_q32, &high);
    return static_cast<crd::i64>((high << 32U) | (low >> 32U));
#else
    const crd::u64 t_lo = ticks & 0xFFFFFFFFULL;
    const crd::u64 t_hi = ticks >> 32U;
    return static_cast<crd::i64>((t_hi * rate_q32) + ((t_lo * rate_q32) >> 32U));
#endif
}

crd::u64 to_q32(crd::f64 ns_per_tick) noexcept
{
    // Truncated: 2^-32 ns a tick is far below the slope's own error.
    return static_cast<crd::u64>(ns_per_tick * 4294967296.0);
}

enum State : crd::u8
{
    Unstarted   = 0U, // no read yet
    Calibrating = 1U, // the origin is set; reads are MonotonicClock's until kCalibrationNs have passed
    Mapping     = 2U, // one reader is publishing the first map; reads are still MonotonicClock's
    Mapped      = 3U, // reads are the counter's
    System      = 4U, // the counter is not invariant: reads are MonotonicClock's for good
};

// The map, published as a seqlock (odd while a re-anchor writes it) so a reader never combines two anchors; every
// field is an atomic, read relaxed between the two sequence loads.
struct alignas(64) TickMap
{
    std::atomic<crd::u32> state{Unstarted};
    std::atomic<crd::u32> seq{0U};
    std::atomic<crd::u64> anchor_ticks{0U};
    std::atomic<crd::i64> anchor_ns{0};
    std::atomic<crd::u64> rate_q32{0U};     // nanoseconds per tick, 32.32 fixed point
    std::atomic<crd::u64> period_ticks{0U}; // the anchor's lifetime, kReanchorNs in ticks
    std::atomic<crd::u64> origin_ticks{0U}; // the calibration's start, the slope's baseline
    std::atomic<crd::i64> origin_ns{0};
    char                  pad[64U - (2U * sizeof(std::atomic<crd::u32>)) - (6U * sizeof(std::atomic<crd::u64>))]{};
};
static_assert(sizeof(TickMap) == 64U, "the map is one cache line, shared by nothing else");

TickMap g_tick_map;

// The mapped read's loads: functions of this unit, so every read inlines them. With MSVC on x64 they are the
// compiler's own volatile loads (what std::atomic's acquire and relaxed loads are there) and a compiler barrier, not
// std::atomic's load: link-time code generation keeps one body of each inline function for the whole program, and when
// that body came from a unit compiled with /arch:AVX2 (every consumer of crd-math, the executable's own units first) it
// is not inlined into a unit built for the baseline ISA, such as this one. std::atomic's load then stayed an
// out-of-line call, six of them in the mapped read, which made TickClock slower than steady_clock on the four-thread
// jobs frame although it is the cheaper read (DIAG.11b (q2), docs/bench/2026-10-26-diag-11b-tick-clock-inlining.md).
// Everywhere else (GCC and Clang inline std::atomic across their link-time optimisation) std::atomic's own loads.
#if defined(_MSC_VER) && defined(_M_X64)
static_assert(sizeof(std::atomic<crd::u32>) == sizeof(crd::u32) && std::atomic<crd::u32>::is_always_lock_free,
              "a std::atomic<u32> is its value");
static_assert(sizeof(std::atomic<crd::u64>) == sizeof(crd::u64) && std::atomic<crd::u64>::is_always_lock_free,
              "a std::atomic<u64> is its value");
static_assert(sizeof(std::atomic<crd::i64>) == sizeof(crd::i64) && std::atomic<crd::i64>::is_always_lock_free,
              "a std::atomic<i64> is its value");

// An acquire fence: on x64 only the compiler may reorder loads, so it is a compiler barrier (std::atomic_thread_fence's
// acquire there).
CRD_FORCEINLINE void acquire_fence() noexcept
{
#pragma warning(suppress : 4996) // deprecated as a hardware fence; it is used here only as the compiler barrier it is
    _ReadWriteBarrier();
}

CRD_FORCEINLINE crd::u32 load_acquire(const std::atomic<crd::u32>& a) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the atomic's storage, as std::atomic reads it
    const crd::u32 value = static_cast<crd::u32>(__iso_volatile_load32(reinterpret_cast<const volatile int*>(&a)));
    acquire_fence(); // x64 loads are acquire loads; later accesses stay after this one
    return value;
}

CRD_FORCEINLINE crd::u32 load_relaxed(const std::atomic<crd::u32>& a) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the atomic's storage, as std::atomic reads it
    return static_cast<crd::u32>(__iso_volatile_load32(reinterpret_cast<const volatile int*>(&a)));
}

template <typename T>
CRD_FORCEINLINE T load_relaxed(const std::atomic<T>& a) noexcept
{
    static_assert(sizeof(T) == 8U, "the map's 64-bit words");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the atomic's storage, as std::atomic reads it
    return static_cast<T>(__iso_volatile_load64(reinterpret_cast<const volatile __int64*>(&a)));
}
#else
CRD_FORCEINLINE void acquire_fence() noexcept
{
    std::atomic_thread_fence(std::memory_order_acquire);
}

template <typename T>
CRD_FORCEINLINE T load_acquire(const std::atomic<T>& a) noexcept
{
    return a.load(std::memory_order_acquire);
}

template <typename T>
CRD_FORCEINLINE T load_relaxed(const std::atomic<T>& a) noexcept
{
    return a.load(std::memory_order_relaxed);
}
#endif

// A tick reading paired with MonotonicClock: the counter read on both sides of the clock, their midpoint kept.
struct Pair
{
    crd::u64 ticks = 0U;
    crd::i64 ns    = 0;
};

Pair read_pair() noexcept
{
    // The tightest of a few: a pair's error is up to half the ticks the MonotonicClock read took, which a preemption
    // or a slow clock (ThreadSanitizer intercepts it) stretches to microseconds; the narrowest bracket keeps the anchor
    // and the slope's two ends close to MonotonicClock (2026-10-26: one pair per anchor missed by 20 us under TSan).
    constexpr int tries = 8;
    Pair     best;
    crd::u64 best_spread = ~crd::u64{0};
    for (int i = 0; i < tries; ++i)
    {
        const crd::u64 before = CycleCounter::now();
        const crd::i64 ns     = MonotonicClock::now().ns_since_epoch();
        const crd::u64 after  = CycleCounter::now();
        const crd::u64 spread = after - before;
        if (spread < best_spread)
        {
            best_spread = spread;
            best        = Pair{before + (spread / 2U), ns};
        }
    }
    return best;
}

// The slope over everything since the calibration began, in nanoseconds per tick; 0 when no tick has passed.
crd::f64 measured_slope(const Pair& now) noexcept
{
    const crd::u64 t0 = g_tick_map.origin_ticks.load(std::memory_order_relaxed);
    const crd::i64 n0 = g_tick_map.origin_ns.load(std::memory_order_relaxed);
    if (now.ticks <= t0 || now.ns <= n0)
    {
        return 0.0;
    }
    return static_cast<crd::f64>(now.ns - n0) / static_cast<crd::f64>(now.ticks - t0);
}

// The reads before the first map: MonotonicClock's, while this moves the state on. Out of line, as the re-anchor is,
// so the mapped read keeps no frame for the calibration it never runs.
CRD_NOINLINE crd::i64 unmapped_now(crd::u32 state) noexcept
{
    if (state == System)
    {
        return MonotonicClock::now().ns_since_epoch();
    }
    if (state == Unstarted)
    {
        crd::u32 expected = Unstarted;
        const bool invariant = counter_invariant();
        if (g_tick_map.state.compare_exchange_strong(expected, Mapping, std::memory_order_acq_rel))
        {
            const Pair origin = read_pair();
            g_tick_map.origin_ticks.store(origin.ticks, std::memory_order_relaxed);
            g_tick_map.origin_ns.store(origin.ns, std::memory_order_relaxed);
            g_tick_map.state.store(invariant ? Calibrating : System, std::memory_order_release);
            return origin.ns;
        }
        return MonotonicClock::now().ns_since_epoch();
    }
    // Calibrating: MonotonicClock alone until the span has passed, then the counter is read beside it.
    const crd::i64 mono   = MonotonicClock::now().ns_since_epoch();
    const crd::i64 origin = g_tick_map.origin_ns.load(std::memory_order_relaxed);
    if (state != Calibrating || mono - origin < TickClock::kCalibrationNs)
    {
        return mono;
    }
    const Pair now      = read_pair();
    crd::u32   expected = Calibrating;
    if (!g_tick_map.state.compare_exchange_strong(expected, Mapping, std::memory_order_acq_rel))
    {
        return now.ns;
    }
    const crd::f64 slope = measured_slope(now);
    if (slope <= 0.0)
    {
        g_tick_map.state.store(Calibrating, std::memory_order_release);
        return now.ns;
    }
    g_tick_map.anchor_ticks.store(now.ticks, std::memory_order_relaxed);
    g_tick_map.anchor_ns.store(now.ns, std::memory_order_relaxed);
    g_tick_map.rate_q32.store(to_q32(slope), std::memory_order_relaxed);
    g_tick_map.period_ticks.store(static_cast<crd::u64>(static_cast<crd::f64>(TickClock::kReanchorNs) / slope),
                                  std::memory_order_relaxed);
    g_tick_map.state.store(Mapped, std::memory_order_release);
    return now.ns;
}

// Re-anchor at MonotonicClock, by the reader that found the anchor `seq` older than its period; another reader
// already re-anchoring makes this one return at once. The new anchor never lies below where the map stood; when the
// map ran ahead, the next period's slope is lowered by the lead (at most halved) so MonotonicClock catches up.
CRD_NOINLINE void reanchor(crd::u32 seq) noexcept
{
    if (!g_tick_map.seq.compare_exchange_strong(seq, seq + 1U, std::memory_order_acquire,
                                                std::memory_order_relaxed))
    {
        return;
    }
    std::atomic_thread_fence(std::memory_order_release);
    const Pair     now     = read_pair();
    const crd::u64 at      = g_tick_map.anchor_ticks.load(std::memory_order_relaxed);
    const crd::i64 ns_at   = g_tick_map.anchor_ns.load(std::memory_order_relaxed);
    const crd::u64 rate    = g_tick_map.rate_q32.load(std::memory_order_relaxed);
    const crd::u64 period  = g_tick_map.period_ticks.load(std::memory_order_relaxed);
    const crd::i64 mapped  = ns_at + scale(now.ticks > at ? now.ticks - at : 0U, rate);
    const crd::i64 anchor  = mapped > now.ns ? mapped : now.ns;
    crd::f64       slope   = measured_slope(now);
    if (slope > 0.0)
    {
        const crd::f64 lead = static_cast<crd::f64>(anchor - now.ns) / static_cast<crd::f64>(period);
        slope               = (slope - lead) > (slope * 0.5) ? (slope - lead) : (slope * 0.5);
        g_tick_map.rate_q32.store(to_q32(slope), std::memory_order_relaxed);
    }
    g_tick_map.anchor_ticks.store(now.ticks, std::memory_order_relaxed);
    g_tick_map.anchor_ns.store(anchor, std::memory_order_relaxed);
    g_tick_map.seq.store(seq + 2U, std::memory_order_release);
}

} // namespace

crd::i64 TickClock::now_ns() noexcept
{
    const crd::u32 state = load_acquire(g_tick_map.state);
    if (state != Mapped)
    {
        return unmapped_now(state);
    }
    for (;;)
    {
        const crd::u32 seq = load_acquire(g_tick_map.seq);
        if ((seq & 1U) != 0U) // a re-anchor is writing: it takes a MonotonicClock read
        {
            continue;
        }
        const crd::u64 at     = load_relaxed(g_tick_map.anchor_ticks);
        const crd::i64 ns_at  = load_relaxed(g_tick_map.anchor_ns);
        const crd::u64 rate   = load_relaxed(g_tick_map.rate_q32);
        const crd::u64 period = load_relaxed(g_tick_map.period_ticks);
        const crd::u64 ticks  = CycleCounter::now();
        acquire_fence(); // the map's loads above stay before the sequence's re-read
        if (load_relaxed(g_tick_map.seq) != seq)
        {
            continue;
        }
        const crd::u64 elapsed = ticks > at ? ticks - at : 0U;
        if (elapsed >= period)
        {
            reanchor(seq);
        }
        return ns_at + scale(elapsed, rate);
    }
}

void TickClock::warm() noexcept
{
    // Read until the state settles: at most kCalibrationNs of reads, once per process (a mapped clock stays mapped).
    crd::u32 state = g_tick_map.state.load(std::memory_order_acquire);
    while (state != Mapped && state != System)
    {
        (void)TickClock::now_ns();
        state = g_tick_map.state.load(std::memory_order_acquire);
    }
}

bool TickClock::prefer_system() noexcept
{
    // Only before the first read: a mapped clock may run ahead of MonotonicClock by its slope's error, so moving its
    // readers onto MonotonicClock afterwards could step them back.
    crd::u32 expected = Unstarted;
    if (g_tick_map.state.compare_exchange_strong(expected, System, std::memory_order_acq_rel))
    {
        return true;
    }
    return expected == System;
}

bool TickClock::counter_usable() noexcept
{
    return counter_invariant();
}

bool TickClock::counter_mapped() noexcept
{
    return g_tick_map.state.load(std::memory_order_acquire) == Mapped;
}

#if CRD_ENABLE_ASSERTS
void TickClock::test_force_map(crd::u64 ticks_at, crd::i64 ns_at, crd::f64 ns_per_tick) noexcept
{
    g_tick_map.anchor_ticks.store(ticks_at, std::memory_order_relaxed);
    g_tick_map.anchor_ns.store(ns_at, std::memory_order_relaxed);
    g_tick_map.rate_q32.store(to_q32(ns_per_tick), std::memory_order_relaxed);
    g_tick_map.period_ticks.store(~crd::u64{0}, std::memory_order_relaxed);
    g_tick_map.seq.store(0U, std::memory_order_relaxed);
    g_tick_map.state.store(Mapped, std::memory_order_release);
}

void TickClock::test_reset() noexcept
{
    g_tick_map.seq.store(0U, std::memory_order_relaxed);
    g_tick_map.period_ticks.store(0U, std::memory_order_relaxed);
    g_tick_map.state.store(Unstarted, std::memory_order_release);
}
#endif

} // namespace crd::time
