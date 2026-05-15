// ---------------------------------------------------------------------------
// crd-perf -- Profiler substrate implementation (Detour D-003 v0a).
//
// One singleton ProfilerState, allocated on init() and destroyed on
// shutdown(). All hot-path state lives there. Per-thread rings are held in
// a fixed-size array indexed by an OS-thread index assigned at
// register_thread().
//
// Hot path (push_region / pop_region):
//
//   1. Read thread-local m_state_ptr (one indirect load; cached on the
//      thread the first time push_region is called).
//   2. Read MonotonicClock::now() (one std::chrono::steady_clock::now()
//      via crd-time -- ~30-100 ns on Windows / Linux).
//   3. Write a 32-byte Sample to the per-thread ring at the SPSC writer
//      position. Wraparound is mod by the constant-power-of-two slot
//      count -- one mask operation.
//
// Name interning:
//
//   The macro CRD_PERF_SCOPE caches the NameId in a TU-local static, so
//   intern_name is called once per call site at first hit. The internal
//   table is a linear-probe hash map (FNV-1a hash on the literal content)
//   protected by a single mutex on insert; reads on the cold path are
//   under the same mutex. Hot-path readers never touch this table -- the
//   NameId travels through the macro's static.
// ---------------------------------------------------------------------------

#include <crd/perf/profiler.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/counters.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/sample.hpp>
#include <crd/time/clocks.hpp>

#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <new>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace detail
{

// ---- Per-thread ring buffer --------------------------------------------
//
// SPSC: writer = the recording thread; reader = the snapshot path (called
// from frame_mark, capture flush, or clear_samples). The reader takes a
// quiescent snapshot of head + tail under acquire ordering; samples
// between them are stable until the writer advances head past tail again.
//
// Slot count is a compile-time power-of-two (kPerThreadRingSlots = 4096).

// MSVC C4324: structure padded due to alignment specifier. Expected -- the
// cache-line alignment is deliberate. Same suppression pattern as
// crd-jobs::ThreadState in scheduler.hpp.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct alignas(64) ThreadRing
{
    Sample* samples = nullptr;      // owning; size = kPerThreadRingSlots
    std::atomic<crd::u64> head{0};  // monotonic writer cursor
    std::atomic<crd::u64> tail{0};  // monotonic reader cursor; cleared by clear_samples
    std::atomic<crd::u64> dropped{0};
    const char* name = nullptr;     // pointer to a string literal (or interned)
    crd::u32 fiber_id_current = 0U; // set via set_current_fiber_id; consumed by push_region
    crd::u32 depth = 0U;            // nesting depth
    bool active = false;            // true between register_thread and clear_thread
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---- Interned name table -----------------------------------------------

struct NameEntry
{
    const char* string = nullptr;
    crd::u32    hash   = 0U;
};

// ---- Counter table -----------------------------------------------------

struct CounterEntry
{
    const char* name = nullptr;
    CounterKind kind = CounterKind::Set;
    CounterType type = CounterType::I64;
    std::atomic<crd::u64> bits{0U}; // raw bits: i64 sign-extended / f64 bit_cast / i64 ns for Duration
};

// ---- Allocator table ---------------------------------------------------

struct AllocatorEntry
{
    const char*              name      = nullptr;
    crd::memory::IAllocator* allocator = nullptr;
};

struct ProfilerState
{
    // Per-thread rings. Indexed by the OS-thread index returned from
    // register_thread() (a u8 -> kMaxThreads).
    ThreadRing rings[kMaxThreads]{};

    // Number of registered threads.
    std::atomic<crd::u32> thread_count{0U};

    // Interned-name table (FNV-1a, linear probe, mutex-protected on insert).
    NameEntry* names = nullptr;
    crd::u32   names_capacity = 0U;   // power-of-two
    crd::u32   names_count    = 0U;   // protected by names_mutex
    std::mutex names_mutex;

    // Counter table (fixed-size, sequentially-indexed; protected by counter_mutex
    // on insert; lookup-by-name uses linear scan -- cold path, table is small).
    CounterEntry* counters = nullptr;            // size = kMaxCounters
    std::atomic<crd::u32> counter_count_atomic{0U}; // monotonically grows
    std::mutex counter_mutex;

    // Allocator table (fixed-size). Slots can be unregistered (allocator
    // pointer set to nullptr); the count never shrinks. Protected by
    // alloc_mutex on insert / unregister.
    AllocatorEntry* allocators = nullptr;          // size = kMaxAllocators
    std::atomic<crd::u32> allocator_count_atomic{0U}; // high-water of registered slots
    std::mutex alloc_mutex;

    // Rolling frame-history ring. Producer = frame_mark() (one thread); reader
    // can be the UI thread or capture thread.
    FrameRecord* frame_history = nullptr;         // size = frame_history_slots
    crd::u32     frame_history_slots = kFrameHistorySlots;
    std::atomic<crd::u64> frame_history_head{0U}; // total frames captured; index via head % slots
    crd::i64     last_frame_end_ns = 0;           // bookkeeping for FrameRecord.frame_begin_ns

    // Per-thread ring slot count (configurable via InitConfig).
    crd::u32 per_thread_ring_slots = kPerThreadRingSlots;

    // Frame bookkeeping. UI consumers read these via current_frame_index().
    std::atomic<crd::u64> frame_count{0U};

    // Initialised flag.
    bool initialised = false;
};

// Singleton. Allocated on init(), zeroed on shutdown(). Pointer access
// only -- the hot-path code reaches through this pointer once per call.
ProfilerState* g_state = nullptr;

// Thread-local: the calling thread's index in g_state->rings, or kInvalidThread
// if not registered.
constexpr crd::u8 kInvalidThread = 0xFFU;
thread_local crd::u8 t_thread_index = kInvalidThread;

// Thread-local: cached pointer to the calling thread's ring. nullptr if the
// thread has not been registered or after shutdown.
thread_local ThreadRing* t_ring = nullptr;

// FNV-1a string hash (deterministic across compilers/platforms).
[[nodiscard]] crd::u32 fnv1a(const char* s) noexcept
{
    crd::u32 h = 0x811C9DC5U;
    while (*s != '\0')
    {
        h ^= static_cast<crd::u8>(*s++);
        h *= 0x01000193U;
    }
    return h;
}

[[nodiscard]] bool is_power_of_two(crd::u32 v) noexcept { return v != 0U && (v & (v - 1U)) == 0U; }

[[nodiscard]] crd::u32 next_power_of_two(crd::u32 v) noexcept
{
    if (v <= 1U)
    {
        return 1U;
    }
    --v;
    v |= v >> 1U;
    v |= v >> 2U;
    v |= v >> 4U;
    v |= v >> 8U;
    v |= v >> 16U;
    return v + 1U;
}

} // namespace detail

using detail::g_state;
using detail::t_ring;
using detail::t_thread_index;

// ---- Init / shutdown ----------------------------------------------------

void init(const InitConfig& cfg)
{
    if (g_state != nullptr)
    {
        return; // idempotent
    }

    auto* state = new detail::ProfilerState();

    state->per_thread_ring_slots = cfg.per_thread_ring_slots != 0U ? cfg.per_thread_ring_slots
                                                                   : kPerThreadRingSlots;
    CRD_ASSERT_MSG(detail::is_power_of_two(state->per_thread_ring_slots),
                   "crd-perf: per_thread_ring_slots must be a power of two");

    const crd::u32 names_cap_req = cfg.max_region_names != 0U ? cfg.max_region_names : kMaxRegionNames;
    state->names_capacity = detail::next_power_of_two(names_cap_req);
    state->names = new detail::NameEntry[state->names_capacity]{};
    state->names_count = 0U;

    // Counter table (fixed size; counters never unregister).
    state->counters = new detail::CounterEntry[kMaxCounters]{};
    state->counter_count_atomic.store(0U, std::memory_order_relaxed);

    // Allocator table (fixed size).
    state->allocators = new detail::AllocatorEntry[kMaxAllocators]{};
    state->allocator_count_atomic.store(0U, std::memory_order_relaxed);

    // Frame-history ring. Use cfg.frame_history_slots if non-zero.
    state->frame_history_slots = cfg.frame_history_slots != 0U ? cfg.frame_history_slots
                                                              : kFrameHistorySlots;
    state->frame_history = new FrameRecord[state->frame_history_slots]{};
    state->frame_history_head.store(0U, std::memory_order_relaxed);
    state->last_frame_end_ns = crd::time::MonotonicClock::now().ns_since_epoch();

    // Allocate per-thread rings lazily on register_thread(); leave them
    // nullptr here so unregistered slots are visibly inert.

    state->initialised = true;
    g_state = state;

    // Auto-register the calling thread as "main".
    (void)register_thread("main");
}

// Forward decl: lives in gpu_scope.cpp. Called from shutdown() to clear
// the cached GPU backend pointer + gpu thread index before the thread
// table is torn down.
namespace detail
{
void reset_gpu_state() noexcept;
} // namespace detail

void shutdown()
{
    if (g_state == nullptr)
    {
        return;
    }

    detail::reset_gpu_state();

    detail::ProfilerState* state = g_state;
    g_state = nullptr; // hot-path observers see nullptr after this point

    for (crd::u32 i = 0U; i < kMaxThreads; ++i)
    {
        delete[] state->rings[i].samples;
        state->rings[i].samples = nullptr;
    }
    delete[] state->names;
    state->names = nullptr;
    delete[] state->counters;
    state->counters = nullptr;
    delete[] state->allocators;
    state->allocators = nullptr;
    delete[] state->frame_history;
    state->frame_history = nullptr;
    delete state;

    // Clear thread-local caches in case the calling thread keeps running.
    t_ring = nullptr;
    t_thread_index = detail::kInvalidThread;
}

[[nodiscard]] bool is_active() noexcept { return g_state != nullptr; }

// ---- Name interning -----------------------------------------------------

[[nodiscard]] NameId intern_name(const char* static_name) noexcept
{
    if (g_state == nullptr || static_name == nullptr)
    {
        return kInvalidNameId;
    }

    detail::ProfilerState& state = *g_state;
    const crd::u32 h = detail::fnv1a(static_name);
    const crd::u32 mask = state.names_capacity - 1U;

    std::lock_guard<std::mutex> lock(state.names_mutex);

    // Linear-probe lookup. We re-hash here under the mutex to keep the
    // logic local; the macro's TU-local cache means this path runs once
    // per call site.
    crd::u32 idx = h & mask;
    for (crd::u32 step = 0U; step < state.names_capacity; ++step)
    {
        detail::NameEntry& e = state.names[idx];
        if (e.string == nullptr)
        {
            // Free slot -- insert.
            if (state.names_count + 1U >= state.names_capacity)
            {
                // Saturated. Engine-side scope names should fit -- bump
                // kMaxRegionNames if this hits.
                CRD_ASSERT_MSG(false, "crd-perf: interned-name table saturated");
                return kInvalidNameId;
            }
            e.string = static_name;
            e.hash   = h;
            ++state.names_count;
            return NameId{idx};
        }
        if (e.hash == h && std::strcmp(e.string, static_name) == 0)
        {
            return NameId{idx};
        }
        idx = (idx + 1U) & mask;
    }
    return kInvalidNameId;
}

[[nodiscard]] const char* resolve_name(NameId id) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= g_state->names_capacity)
    {
        return "";
    }
    const detail::NameEntry& e = g_state->names[id.value];
    return e.string != nullptr ? e.string : "";
}

[[nodiscard]] crd::u32 intern_name_capacity() noexcept
{
    return g_state != nullptr ? g_state->names_capacity : 0U;
}

[[nodiscard]] crd::u32 intern_name_count() noexcept
{
    if (g_state == nullptr)
    {
        return 0U;
    }
    // names_count is mutated under names_mutex; relaxed read is safe for
    // capture sizing (saturated upper bound). The reader will validate.
    std::lock_guard<std::mutex> lock(g_state->names_mutex);
    return g_state->names_count;
}

// ---- Thread registration ------------------------------------------------

crd::u8 register_thread(const char* name) noexcept
{
    if (g_state == nullptr)
    {
        return detail::kInvalidThread;
    }

    // Already registered? Refresh the t_ring cache and return.
    if (t_thread_index != detail::kInvalidThread)
    {
        t_ring = &g_state->rings[t_thread_index];
        if (name != nullptr)
        {
            t_ring->name = name;
        }
        return t_thread_index;
    }

    const crd::u32 idx = g_state->thread_count.fetch_add(1U, std::memory_order_acq_rel);
    if (idx >= kMaxThreads)
    {
        g_state->thread_count.fetch_sub(1U, std::memory_order_acq_rel);
        CRD_ASSERT_MSG(false, "crd-perf: kMaxThreads exceeded");
        return detail::kInvalidThread;
    }

    detail::ThreadRing& ring = g_state->rings[idx];
    ring.samples           = new Sample[g_state->per_thread_ring_slots]{};
    ring.head.store(0U, std::memory_order_relaxed);
    ring.tail.store(0U, std::memory_order_relaxed);
    ring.dropped.store(0U, std::memory_order_relaxed);
    ring.name              = name;
    ring.fiber_id_current  = 0U;
    ring.depth             = 0U;
    ring.active            = true;

    t_thread_index = static_cast<crd::u8>(idx);
    t_ring         = &ring;
    return t_thread_index;
}

[[nodiscard]] crd::u8 current_thread_index() noexcept { return t_thread_index; }

void set_current_fiber_id(crd::u32 fiber_id) noexcept
{
    if (t_ring != nullptr)
    {
        t_ring->fiber_id_current = fiber_id;
    }
}

[[nodiscard]] crd::u32 current_fiber_id() noexcept
{
    return t_ring != nullptr ? t_ring->fiber_id_current : 0U;
}

// ---- Scope push / pop (hot path) ---------------------------------------

[[nodiscard]] BeginToken push_region(NameId /*id*/, Category /*cat*/, crd::u32 /*color_rgba*/) noexcept
{
    BeginToken token{};
    detail::ThreadRing* ring = t_ring;
    if (ring == nullptr)
    {
        return token;
    }
    token.begin_ns     = crd::time::MonotonicClock::now().ns_since_epoch();
    token.begin_fiber  = ring->fiber_id_current;
    token.begin_thread = t_thread_index;
    token.depth        = static_cast<crd::u8>(ring->depth);
    ++ring->depth;
    return token;
}

void pop_region(NameId id, BeginToken begin, Category cat, crd::u32 color_rgba) noexcept
{
    detail::ThreadRing* ring = t_ring;
    if (ring == nullptr)
    {
        return;
    }

    const crd::i64 end_ns     = crd::time::MonotonicClock::now().ns_since_epoch();
    const crd::u8  end_thread = t_thread_index; // catches fiber migration if differs from begin.begin_thread
    const crd::u32 end_fiber  = ring->fiber_id_current;
    (void)end_fiber; // reserved for split BEGIN/END events in v0c

    if (ring->depth > 0U)
    {
        --ring->depth;
    }

    const crd::u32 mask  = g_state->per_thread_ring_slots - 1U;
    const crd::u64 h     = ring->head.load(std::memory_order_relaxed);
    const crd::u64 t     = ring->tail.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->per_thread_ring_slots;

    if (h - t >= slots)
    {
        ring->dropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    Sample& s      = ring->samples[h & mask];
    s.begin_ns     = begin.begin_ns;
    s.end_ns       = end_ns;
    s.name_id      = id.value;
    s.color_rgba   = color_rgba;
    s.begin_thread = begin.begin_thread;
    s.end_thread   = end_thread;
    s.depth        = begin.depth;
    s.category     = static_cast<crd::u8>(cat);
    s.fiber_id     = begin.begin_fiber;

    ring->head.store(h + 1U, std::memory_order_release);
}

// ---- Frame boundary -----------------------------------------------------

void frame_mark() noexcept
{
    if (g_state == nullptr)
    {
        return;
    }
    detail::ProfilerState& state = *g_state;

    const crd::u64 frame_index = state.frame_count.fetch_add(1U, std::memory_order_acq_rel);
    const crd::i64 end_ns      = crd::time::MonotonicClock::now().ns_since_epoch();

    // Snapshot counters into the rolling history ring.
    const crd::u32 history_head_idx = static_cast<crd::u32>(
        state.frame_history_head.load(std::memory_order_relaxed) % state.frame_history_slots);
    FrameRecord& rec      = state.frame_history[history_head_idx];
    rec.frame_index       = frame_index;
    rec.frame_begin_ns    = state.last_frame_end_ns;
    rec.frame_end_ns      = end_ns;
    const crd::u32 n_counters = state.counter_count_atomic.load(std::memory_order_acquire);
    rec.counter_count     = n_counters > kMaxCounters ? kMaxCounters : n_counters;

    for (crd::u32 i = 0U; i < rec.counter_count; ++i)
    {
        detail::CounterEntry& c = state.counters[i];
        const crd::u64 bits = c.bits.load(std::memory_order_relaxed);
        rec.values[i].bits = bits;
        if (c.kind == CounterKind::Add)
        {
            c.bits.store(0U, std::memory_order_relaxed);
        }
    }

    // Snapshot allocator stats. Each allocator's MemoryStats reads are
    // relaxed-atomic loads; the snapshot is "approximately at this frame
    // boundary" (allocators bumping during the snapshot still settle into
    // the next frame). The allocator slot can be empty (unregistered) --
    // we leave the AllocatorRecord zeroed in that case.
    const crd::u32 n_allocs = state.allocator_count_atomic.load(std::memory_order_acquire);
    rec.allocator_count     = n_allocs > kMaxAllocators ? kMaxAllocators : n_allocs;
    for (crd::u32 i = 0U; i < rec.allocator_count; ++i)
    {
        const detail::AllocatorEntry& ae = state.allocators[i];
        AllocatorRecord& ar              = rec.allocators[i];
        if (ae.allocator == nullptr)
        {
            ar = AllocatorRecord{};
            continue;
        }
        const auto snap   = ae.allocator->stats().snapshot();
        ar.alloc_count    = snap.alloc_count;
        ar.dealloc_count  = snap.dealloc_count;
        ar.bytes_in_use   = snap.bytes_in_use;
        ar.peak_bytes     = snap.peak_bytes;
        ar.total_bytes    = snap.total_bytes;
        ar._pad           = 0U;
    }

    // Retire the slot -- readers see this frame's record once the head
    // advances past it (release ordering on the head store).
    state.frame_history_head.fetch_add(1U, std::memory_order_acq_rel);
    state.last_frame_end_ns = end_ns;
}

[[nodiscard]] crd::u64 frame_count() noexcept
{
    return g_state != nullptr ? g_state->frame_count.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] crd::u64 current_frame_index() noexcept { return frame_count(); }

// ---- Introspection -----------------------------------------------------

[[nodiscard]] ThreadSamplesView thread_samples(crd::u8 thread_index) noexcept
{
    ThreadSamplesView view{nullptr, 0U, 0U, nullptr};
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return view;
    }
    detail::ThreadRing& ring = g_state->rings[thread_index];
    if (!ring.active)
    {
        return view;
    }
    const crd::u64 h = ring.head.load(std::memory_order_acquire);
    const crd::u64 t = ring.tail.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->per_thread_ring_slots;
    const crd::u64 count = h - t > slots ? slots : h - t;
    view.data    = ring.samples;
    view.size    = static_cast<crd::u32>(count);
    view.dropped = static_cast<crd::u32>(ring.dropped.load(std::memory_order_relaxed));
    view.name    = ring.name;
    return view;
}

[[nodiscard]] crd::u32 thread_count() noexcept
{
    return g_state != nullptr ? g_state->thread_count.load(std::memory_order_acquire) : 0U;
}

void clear_samples() noexcept
{
    if (g_state == nullptr)
    {
        return;
    }
    for (crd::u32 i = 0U; i < kMaxThreads; ++i)
    {
        detail::ThreadRing& ring = g_state->rings[i];
        if (!ring.active)
        {
            continue;
        }
        const crd::u64 h = ring.head.load(std::memory_order_acquire);
        ring.tail.store(h, std::memory_order_release);
        ring.dropped.store(0U, std::memory_order_relaxed);
    }
}

// ---- External sample write (backend-internal) --------------------------
//
// Used by the GPU backend (crd-rhi-vulkan VulkanProfilerBackend) to push a
// fully-formed Sample into a target thread's ring after resolving a GPU
// timestamp pair. The writer is the host thread driving resolve, not the
// "gpu" thread itself -- so the standard t_ring fast path is wrong; we
// need explicit thread targeting.
//
// Thread-safety: this is MPSC writing into the target ring (the GPU thread
// itself never writes; the resolver thread is the writer; reader is the UI
// snapshot). The standard ring is SPSC, so multiple concurrent resolvers
// would race. Today only one thread drives resolve (the main thread); if
// that changes the backend must serialise.

namespace detail
{

void write_external_sample(crd::u8 thread_index, const Sample& s) noexcept
{
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return;
    }
    ThreadRing& ring = g_state->rings[thread_index];
    if (!ring.active)
    {
        return;
    }
    const crd::u32 mask  = g_state->per_thread_ring_slots - 1U;
    const crd::u64 h     = ring.head.load(std::memory_order_relaxed);
    const crd::u64 t     = ring.tail.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->per_thread_ring_slots;
    if (h - t >= slots)
    {
        ring.dropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    ring.samples[h & mask] = s;
    ring.head.store(h + 1U, std::memory_order_release);
}

} // namespace detail

// ---- Counter registration (cold path) -----------------------------------

namespace detail
{

[[nodiscard]] CounterId register_counter_impl(const char* static_name, CounterKind kind,
                                              CounterType type) noexcept
{
    if (g_state == nullptr || static_name == nullptr)
    {
        return kInvalidCounterId;
    }
    detail::ProfilerState& state = *g_state;
    std::lock_guard<std::mutex> lock(state.counter_mutex);

    const crd::u32 n = state.counter_count_atomic.load(std::memory_order_relaxed);
    // Dedup by name + kind + type. Three-key dedup keeps "draws" / Set / I64
    // distinct from "draws" / Add / I64 -- a user mistake we surface
    // by giving them different ids rather than aliasing.
    for (crd::u32 i = 0U; i < n; ++i)
    {
        const detail::CounterEntry& e = state.counters[i];
        if (e.name == nullptr)
        {
            continue;
        }
        if (e.kind == kind && e.type == type && std::strcmp(e.name, static_name) == 0)
        {
            return CounterId{i};
        }
    }
    if (n >= kMaxCounters)
    {
        CRD_ASSERT_MSG(false, "crd-perf: kMaxCounters exceeded");
        return kInvalidCounterId;
    }
    detail::CounterEntry& slot = state.counters[n];
    slot.name = static_name;
    slot.kind = kind;
    slot.type = type;
    slot.bits.store(0U, std::memory_order_relaxed);
    state.counter_count_atomic.store(n + 1U, std::memory_order_release);
    return CounterId{n};
}

} // namespace detail

[[nodiscard]] CounterId register_counter_i64(const char* static_name, CounterKind kind) noexcept
{
    return detail::register_counter_impl(static_name, kind, CounterType::I64);
}

[[nodiscard]] CounterId register_counter_f64(const char* static_name, CounterKind kind) noexcept
{
    return detail::register_counter_impl(static_name, kind, CounterType::F64);
}

[[nodiscard]] CounterId register_counter_duration(const char* static_name, CounterKind kind) noexcept
{
    return detail::register_counter_impl(static_name, kind, CounterType::DurationNs);
}

// ---- Counter writes (hot path) ------------------------------------------

void counter_set_i64(CounterId id, crd::i64 value) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    g_state->counters[id.value].bits.store(static_cast<crd::u64>(value), std::memory_order_relaxed);
}

void counter_set_f64(CounterId id, crd::f64 value) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    g_state->counters[id.value].bits.store(std::bit_cast<crd::u64>(value), std::memory_order_relaxed);
}

void counter_set_duration(CounterId id, crd::time::Duration value) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    const crd::i64 ns = static_cast<crd::i64>(value.value * 1.0e9);
    g_state->counters[id.value].bits.store(static_cast<crd::u64>(ns), std::memory_order_relaxed);
}

void counter_add_i64(CounterId id, crd::i64 delta) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    g_state->counters[id.value].bits.fetch_add(static_cast<crd::u64>(delta),
                                               std::memory_order_relaxed);
}

void counter_add_f64(CounterId id, crd::f64 delta) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    // f64 add via CAS-loop (atomic floats aren't lock-free with fetch_add
    // on most ISAs). Relaxed ordering keeps the read-modify-write cheap.
    auto& slot = g_state->counters[id.value].bits;
    crd::u64 expected = slot.load(std::memory_order_relaxed);
    crd::u64 desired  = 0U;
    do
    {
        const crd::f64 cur = std::bit_cast<crd::f64>(expected);
        desired = std::bit_cast<crd::u64>(cur + delta);
    } while (!slot.compare_exchange_weak(expected, desired, std::memory_order_relaxed,
                                         std::memory_order_relaxed));
}

void counter_add_duration(CounterId id, crd::time::Duration delta) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    const crd::i64 ns_delta = static_cast<crd::i64>(delta.value * 1.0e9);
    g_state->counters[id.value].bits.fetch_add(static_cast<crd::u64>(ns_delta),
                                               std::memory_order_relaxed);
}

// ---- Counter introspection ---------------------------------------------

[[nodiscard]] crd::u32 counter_count() noexcept
{
    return g_state != nullptr ? g_state->counter_count_atomic.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] CounterInfo counter_info(CounterId id) noexcept
{
    CounterInfo info{"", CounterKind::Set, CounterType::I64};
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return info;
    }
    const detail::CounterEntry& e = g_state->counters[id.value];
    info.name = e.name != nullptr ? e.name : "";
    info.kind = e.kind;
    info.type = e.type;
    return info;
}

[[nodiscard]] crd::i64 counter_current_i64(CounterId id) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return 0;
    }
    return static_cast<crd::i64>(g_state->counters[id.value].bits.load(std::memory_order_relaxed));
}

[[nodiscard]] crd::f64 counter_current_f64(CounterId id) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return 0.0;
    }
    return std::bit_cast<crd::f64>(g_state->counters[id.value].bits.load(std::memory_order_relaxed));
}

[[nodiscard]] crd::time::Duration counter_current_duration(CounterId id) noexcept
{
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return crd::time::Duration{};
    }
    const crd::i64 ns = static_cast<crd::i64>(
        g_state->counters[id.value].bits.load(std::memory_order_relaxed));
    return crd::time::Duration{static_cast<crd::f64>(ns) * 1.0e-9};
}

// ---- Allocator registry (cold path) ------------------------------------

[[nodiscard]] crd::u32 register_allocator(const char* name,
                                          crd::memory::IAllocator* allocator) noexcept
{
    if (g_state == nullptr || name == nullptr || allocator == nullptr)
    {
        return kInvalidAllocatorIdx;
    }
    detail::ProfilerState& state = *g_state;
    std::lock_guard<std::mutex> lock(state.alloc_mutex);

    const crd::u32 n = state.allocator_count_atomic.load(std::memory_order_relaxed);

    // Dedup by allocator pointer; same instance always returns the same idx.
    // Re-registering after unregister overrides any cleared slot.
    for (crd::u32 i = 0U; i < n; ++i)
    {
        if (state.allocators[i].allocator == allocator)
        {
            // Already registered -- refresh the name in case the caller
            // wants to relabel.
            state.allocators[i].name = name;
            return i;
        }
    }
    // Re-use a cleared slot (unregister leaves a hole) before appending.
    for (crd::u32 i = 0U; i < n; ++i)
    {
        if (state.allocators[i].allocator == nullptr)
        {
            state.allocators[i].name      = name;
            state.allocators[i].allocator = allocator;
            return i;
        }
    }
    if (n >= kMaxAllocators)
    {
        CRD_ASSERT_MSG(false, "crd-perf: kMaxAllocators exceeded");
        return kInvalidAllocatorIdx;
    }
    state.allocators[n].name      = name;
    state.allocators[n].allocator = allocator;
    state.allocator_count_atomic.store(n + 1U, std::memory_order_release);
    return n;
}

void unregister_allocator(crd::u32 allocator_idx) noexcept
{
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return;
    }
    detail::ProfilerState& state = *g_state;
    std::lock_guard<std::mutex> lock(state.alloc_mutex);
    if (allocator_idx >= state.allocator_count_atomic.load(std::memory_order_relaxed))
    {
        return;
    }
    state.allocators[allocator_idx].name      = nullptr;
    state.allocators[allocator_idx].allocator = nullptr;
    // High-water (allocator_count_atomic) stays unchanged -- slots become
    // re-usable but the count never shrinks (UI sees a stable indexing).
}

[[nodiscard]] crd::u32 registered_allocator_count() noexcept
{
    return g_state != nullptr ? g_state->allocator_count_atomic.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] AllocatorInfo allocator_info(crd::u32 allocator_idx) noexcept
{
    AllocatorInfo info{};
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return info;
    }
    if (allocator_idx >= g_state->allocator_count_atomic.load(std::memory_order_acquire))
    {
        return info;
    }
    const detail::AllocatorEntry& e = g_state->allocators[allocator_idx];
    info.name      = e.name != nullptr ? e.name : "";
    info.allocator = e.allocator;
    return info;
}

[[nodiscard]] AllocatorSnapshot allocator_snapshot(crd::u32 allocator_idx) noexcept
{
    AllocatorSnapshot snap{};
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return snap;
    }
    if (allocator_idx >= g_state->allocator_count_atomic.load(std::memory_order_acquire))
    {
        return snap;
    }
    const detail::AllocatorEntry& e = g_state->allocators[allocator_idx];
    if (e.allocator == nullptr)
    {
        return snap;
    }
    const auto raw = e.allocator->stats().snapshot();
    snap.name           = e.name != nullptr ? e.name : "";
    snap.alloc_count    = raw.alloc_count;
    snap.dealloc_count  = raw.dealloc_count;
    snap.bytes_in_use   = raw.bytes_in_use;
    snap.peak_bytes     = raw.peak_bytes;
    snap.total_bytes    = raw.total_bytes;
    return snap;
}

[[nodiscard]] AllocatorSnapshot allocator_snapshot_history(crd::u32 allocator_idx,
                                                           crd::u32 frames_back) noexcept
{
    AllocatorSnapshot snap{};
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return snap;
    }
    const FrameRecord* rec = frame_record(frames_back);
    if (rec == nullptr || allocator_idx >= rec->allocator_count)
    {
        return snap;
    }
    if (allocator_idx >= g_state->allocator_count_atomic.load(std::memory_order_acquire))
    {
        return snap;
    }
    const detail::AllocatorEntry& e  = g_state->allocators[allocator_idx];
    const AllocatorRecord&        ar = rec->allocators[allocator_idx];
    snap.name           = e.name != nullptr ? e.name : "";
    snap.alloc_count    = ar.alloc_count;
    snap.dealloc_count  = ar.dealloc_count;
    snap.bytes_in_use   = ar.bytes_in_use;
    snap.peak_bytes     = ar.peak_bytes;
    snap.total_bytes    = ar.total_bytes;
    return snap;
}

// ---- Frame-record introspection -----------------------------------------

[[nodiscard]] const FrameRecord* frame_record(crd::u32 frames_back) noexcept
{
    if (g_state == nullptr)
    {
        return nullptr;
    }
    detail::ProfilerState& state = *g_state;
    const crd::u64 head = state.frame_history_head.load(std::memory_order_acquire);
    if (head == 0U || frames_back >= state.frame_history_slots || frames_back >= head)
    {
        return nullptr;
    }
    const crd::u64 target = head - 1U - static_cast<crd::u64>(frames_back);
    const crd::u32 idx    = static_cast<crd::u32>(target % state.frame_history_slots);
    return &state.frame_history[idx];
}

[[nodiscard]] crd::u32 frame_record_count() noexcept
{
    if (g_state == nullptr)
    {
        return 0U;
    }
    const crd::u64 head  = g_state->frame_history_head.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->frame_history_slots;
    return static_cast<crd::u32>(head < slots ? head : slots);
}

#else // CRD_PERF_ENABLED == 0

// All stubs below the gate. Identical signatures so consumer headers and
// the unit tests compile in either configuration.

void init(const InitConfig&) {}
void shutdown() {}
[[nodiscard]] bool is_active() noexcept { return false; }

[[nodiscard]] NameId intern_name(const char*) noexcept { return kInvalidNameId; }
[[nodiscard]] const char* resolve_name(NameId) noexcept { return ""; }
[[nodiscard]] crd::u32 intern_name_capacity() noexcept { return 0U; }
[[nodiscard]] crd::u32 intern_name_count() noexcept { return 0U; }

crd::u8  register_thread(const char*) noexcept { return 0xFFU; }
[[nodiscard]] crd::u8  current_thread_index() noexcept { return 0xFFU; }
void                    set_current_fiber_id(crd::u32) noexcept {}
[[nodiscard]] crd::u32 current_fiber_id() noexcept { return 0U; }

[[nodiscard]] crd::u64 frame_count() noexcept { return 0U; }
[[nodiscard]] crd::u64 current_frame_index() noexcept { return 0U; }

[[nodiscard]] ThreadSamplesView thread_samples(crd::u8) noexcept
{
    return ThreadSamplesView{nullptr, 0U, 0U, nullptr};
}

[[nodiscard]] crd::u32 thread_count() noexcept { return 0U; }
void clear_samples() noexcept {}

[[nodiscard]] CounterId register_counter_i64(const char*, CounterKind) noexcept { return kInvalidCounterId; }
[[nodiscard]] CounterId register_counter_f64(const char*, CounterKind) noexcept { return kInvalidCounterId; }
[[nodiscard]] CounterId register_counter_duration(const char*, CounterKind) noexcept
{
    return kInvalidCounterId;
}

[[nodiscard]] crd::u32  counter_count() noexcept { return 0U; }
[[nodiscard]] CounterInfo counter_info(CounterId) noexcept
{
    return CounterInfo{"", CounterKind::Set, CounterType::I64};
}
[[nodiscard]] crd::i64 counter_current_i64(CounterId) noexcept { return 0; }
[[nodiscard]] crd::f64 counter_current_f64(CounterId) noexcept { return 0.0; }
[[nodiscard]] crd::time::Duration counter_current_duration(CounterId) noexcept
{
    return crd::time::Duration{};
}

[[nodiscard]] const FrameRecord* frame_record(crd::u32) noexcept { return nullptr; }
[[nodiscard]] crd::u32 frame_record_count() noexcept { return 0U; }

[[nodiscard]] crd::u32 register_allocator(const char*, crd::memory::IAllocator*) noexcept
{
    return kInvalidAllocatorIdx;
}
void unregister_allocator(crd::u32) noexcept {}
[[nodiscard]] crd::u32 registered_allocator_count() noexcept { return 0U; }
[[nodiscard]] AllocatorInfo allocator_info(crd::u32) noexcept { return AllocatorInfo{}; }
[[nodiscard]] AllocatorSnapshot allocator_snapshot(crd::u32) noexcept { return AllocatorSnapshot{}; }
[[nodiscard]] AllocatorSnapshot allocator_snapshot_history(crd::u32, crd::u32) noexcept
{
    return AllocatorSnapshot{};
}

#endif // CRD_PERF_ENABLED

} // namespace crd::perf
