// crd-time -- clock implementations (Detour D-006).

#include <crd/time/clocks.hpp>
#include <crd/time/deadline.hpp> // yield_thread

#include <crd/core/cpu_features.hpp>
#include <crd/core/platform.hpp> // CRD_NOINLINE
#include <crd/core/wide_multiply.hpp>

#include <atomic>
#include <chrono>

#if defined(_MSC_VER)
    #include <intrin.h>
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
#elif defined(_MSC_VER) && defined(_M_ARM64)
    // CNTVCT_EL0, the generic timer read above as cntvct_el0. The register's name (ARM64_CNTVCT) comes from the
    // SDK's winnt.h, which this unit does not include; the compiler's arm64intr.h gives the encoder.
    return static_cast<crd::u64>(_ReadStatusReg(ARM64_SYSREG(3, 3, 14, 0, 2)));
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

// The counter where TickClock may map it: x86-64's TSC when every CPU reports it invariant (constant rate in every
// P-, C- and T-state), ARM64's generic timer (a constant frequency by architecture); nowhere else.
bool counter_invariant() noexcept
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return crd::cpu_selection().features.usable.has(crd::CpuFeature::InvariantTsc);
#endif
}

// (ticks * ns_per_tick_q32) >> 32: a tick count scaled by a 32.32 fixed-point rate, without overflow for any count.
crd::i64 scale(crd::u64 ticks, crd::u64 rate_q32) noexcept
{
    const crd::WideProduct p = crd::multiply_wide(ticks, rate_q32);
    return static_cast<crd::i64>((p.hi << 32U) | (p.lo >> 32U));
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
    Backwards   = 5U, // the counter fell behind the calibration's start: reads are MonotonicClock's for good
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
    // The latest MonotonicClock reading an unmapped read may have returned: the first map's anchor never lies below it
    // (written only before the first map, so the mapped reads share the line with no writer).
    std::atomic<crd::i64> floor_ns{0};
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
// Everywhere else std::atomic's own loads: GCC's link-time optimisation inlines them (checked on linux-gcc-shipping);
// clang-cl takes the MSVC path above; Clang's own link-time optimisation on Linux was not inspected.
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

// One turn of a reader's wait for a publisher (the first map's or a re-anchor's: a read_pair and a few stores): the
// core's spin-wait hint for the first turns, then the rest of the time slice, so a publisher that lost its processor to
// the waiting readers gets one back.
void wait_turn(crd::u32 turn) noexcept
{
    if (turn >= 64U)
    {
        yield_thread();
        return;
    }
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

// Wait while a re-anchor writes the map (the sequence is odd). Out of line: the mapped read keeps only the call.
CRD_NOINLINE void wait_for_reanchor() noexcept
{
    for (crd::u32 turn = 0U; (load_relaxed(g_tick_map.seq) & 1U) != 0U; ++turn)
    {
        wait_turn(turn);
    }
}

// Wait while a publisher holds the state at Mapping (the origin's read_pair, or the first map's).
void wait_for_mapping() noexcept
{
    for (crd::u32 turn = 0U; load_relaxed(g_tick_map.state) == Mapping; ++turn)
    {
        wait_turn(turn);
    }
}

// Raise the floor to `ns`, by a read-modify-write even when the floor is already higher: the first map's publisher
// reads the floor after its exchange of the state, and the sequentially consistent order places a reader's
// read-modify-write, then its re-read of the state, before that exchange whenever the re-read still finds Calibrating.
void raise_floor(crd::i64 ns) noexcept
{
    crd::i64 floor = g_tick_map.floor_ns.load(std::memory_order_relaxed);
    while (!g_tick_map.floor_ns.compare_exchange_weak(floor, floor > ns ? floor : ns, std::memory_order_seq_cst,
                                                      std::memory_order_relaxed))
    {
        // `floor` now holds the value another reader raised it to: try again from there.
    }
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

// The mapped read: the counter scaled from the anchor of one map, re-anchored by the reader that finds the anchor's
// period over.
CRD_FORCEINLINE crd::i64 mapped_now() noexcept
{
    for (;;)
    {
        const crd::u32 seq = load_acquire(g_tick_map.seq);
        if ((seq & 1U) != 0U) // a re-anchor is writing: it takes a read_pair
        {
            wait_for_reanchor();
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

// The reads before the first map: MonotonicClock's, while this moves the state on. Out of line, as the re-anchor is,
// so the mapped read keeps no frame for the calibration it never runs.
//
// The hand-off to the first map must not step any reader back (2026-10-26: a thread's last MonotonicClock reading,
// on the 100 ns grid of QueryPerformanceCounter, lay up to 37 ns past its first mapped read when the publisher read its
// pair before taking the state, and other threads went on returning MonotonicClock readings until the map was stored;
// docs/sessions/2026-10-26-tick-clock-cross-thread.md). So a MonotonicClock reading is returned only after it raised the
// floor and the state, re-read, still says Calibrating; the publisher takes the state first, then reads its pair and
// the floor, and anchors at the later of the two. Whenever a reader's re-read found Calibrating, its raise comes
// before the publisher's exchange in the sequentially consistent order and the publisher's read of the floor after it,
// so every reading returned lies at or below the anchor, and a mapped read never below the anchor. A reader that finds
// the state taken waits for the publication (one read_pair) instead of returning a reading.
CRD_NOINLINE crd::i64 unmapped_now(crd::u32 state) noexcept
{
    for (;;)
    {
        if (state == Mapped)
        {
            return mapped_now();
        }
        if (state == System || state == Backwards)
        {
            return MonotonicClock::now().ns_since_epoch();
        }
        if (state == Mapping)
        {
            wait_for_mapping();
            state = g_tick_map.state.load(std::memory_order_acquire);
            continue;
        }
        if (state == Unstarted)
        {
            crd::u32   expected  = Unstarted;
            const bool invariant = counter_invariant();
            if (g_tick_map.state.compare_exchange_strong(expected, Mapping, std::memory_order_acq_rel))
            {
                const Pair origin = read_pair();
                g_tick_map.origin_ticks.store(origin.ticks, std::memory_order_relaxed);
                g_tick_map.origin_ns.store(origin.ns, std::memory_order_relaxed);
                raise_floor(origin.ns);
                g_tick_map.state.store(invariant ? Calibrating : System, std::memory_order_release);
                return origin.ns;
            }
            state = expected;
            continue;
        }
        // Calibrating: MonotonicClock alone until the span has passed, then the first map.
        const crd::i64 mono   = MonotonicClock::now().ns_since_epoch();
        const crd::i64 origin = g_tick_map.origin_ns.load(std::memory_order_relaxed);
        if (mono - origin < TickClock::kCalibrationNs)
        {
            raise_floor(mono);
            state = g_tick_map.state.load(std::memory_order_seq_cst);
            if (state == Calibrating)
            {
                return mono;
            }
            continue; // a publisher took the state: this reading may lie past its anchor
        }
        crd::u32 expected = Calibrating;
        if (!g_tick_map.state.compare_exchange_strong(expected, Mapping, std::memory_order_seq_cst))
        {
            state = expected;
            continue;
        }
        const Pair     now   = read_pair();
        const crd::f64 slope = measured_slope(now);
        if (slope <= 0.0)
        {
            // kCalibrationNs of MonotonicClock passed and the counter stands at or below the origin's reading: it ran
            // backwards (a host whose processors' counters disagree, though CPUID calls them invariant), so no map of
            // it can keep a thread from stepping back. Settle on MonotonicClock rather than calibrate on every read.
            g_tick_map.state.store(Backwards, std::memory_order_release);
            return now.ns;
        }
        const crd::i64 floor  = g_tick_map.floor_ns.load(std::memory_order_seq_cst);
        const crd::i64 anchor = now.ns > floor ? now.ns : floor;
        g_tick_map.anchor_ticks.store(now.ticks, std::memory_order_relaxed);
        g_tick_map.anchor_ns.store(anchor, std::memory_order_relaxed);
        g_tick_map.rate_q32.store(to_q32(slope), std::memory_order_relaxed);
        g_tick_map.period_ticks.store(static_cast<crd::u64>(static_cast<crd::f64>(TickClock::kReanchorNs) / slope),
                                      std::memory_order_relaxed);
        g_tick_map.state.store(Mapped, std::memory_order_release);
        return anchor;
    }
}

} // namespace

crd::i64 TickClock::now_ns() noexcept
{
    const crd::u32 state = load_acquire(g_tick_map.state);
    if (state != Mapped)
    {
        return unmapped_now(state);
    }
    return mapped_now();
}

void TickClock::warm() noexcept
{
    // Read until the state settles: at most kCalibrationNs of reads, once per process (a mapped clock stays mapped).
    crd::u32 state = g_tick_map.state.load(std::memory_order_acquire);
    while (state != Mapped && state != System && state != Backwards)
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
    return expected == System || expected == Backwards;
}

bool TickClock::counter_usable() noexcept
{
    return counter_invariant() && g_tick_map.state.load(std::memory_order_acquire) != Backwards;
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

void TickClock::test_calibrate_from(crd::u64 origin_ticks) noexcept
{
    g_tick_map.seq.store(0U, std::memory_order_relaxed);
    g_tick_map.period_ticks.store(0U, std::memory_order_relaxed);
    g_tick_map.origin_ticks.store(origin_ticks, std::memory_order_relaxed);
    g_tick_map.origin_ns.store(MonotonicClock::now().ns_since_epoch(), std::memory_order_relaxed);
    g_tick_map.floor_ns.store(g_tick_map.origin_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_tick_map.state.store(Calibrating, std::memory_order_release);
}

void TickClock::test_reset() noexcept
{
    g_tick_map.seq.store(0U, std::memory_order_relaxed);
    g_tick_map.period_ticks.store(0U, std::memory_order_relaxed);
    g_tick_map.floor_ns.store(0, std::memory_order_relaxed);
    g_tick_map.state.store(Unstarted, std::memory_order_release);
}
#endif

} // namespace crd::time
