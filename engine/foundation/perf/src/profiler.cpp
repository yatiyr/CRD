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
#include <thread> // std::this_thread::yield -- unregister quiescence (DIAG.6a(b))

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

// Publication protocol (DIAG.6a(c1)/(c2)/(c3)): SPSC producer, DROP-ON-FULL. The writer (producer) refuses to write
// when h - t >= slots (bumping `dropped`), so it NEVER overwrites the unread [tail, head) region -- there is no
// torn-payload hazard, and no seqlock is needed here (unlike the overwrite-oldest frame-history ring). A consumer
// copies the live samples [tail, head) OLDEST-FIRST via copy_thread_samples (mask-indexed -- data is NOT contiguous
// from base after a wrap). The zero-copy thread_samples() view is same-thread-only (see its header).
//
// (c3) ENFORCED SINGLE CONSUMER. The c2 disjointness argument (writer touches [head, tail+slots), consumer copies
// [tail, head), disjoint) holds ONLY while `tail` is fixed. Two things move against that: a second concurrent copier,
// and clear_samples (which moves tail to head). So `reader_busy` admits exactly ONE consumer -- copier OR clearer --
// per ring at a time: copy_thread_samples exchange-acquires it and, if already held, refuses (returns 0 and bumps
// `contended`, distinguishable from empty via the out-param); clear_samples spins to acquire it before moving tail.
// The producer never takes the flag (it only moves head; a concurrently-advancing tail from clear just frees space
// for the drop-on-full check). `active` is atomic so a cross-thread consumer observes the samples/head/tail writes
// published before it (release on set, acquire on read).
struct alignas(64) ThreadRing
{
    Sample* samples = nullptr;      // owning; size = kPerThreadRingSlots
    // DIAG.6b(b): optional per-slot correlation, SLOT-PARALLEL to `samples` (same index h & mask). nullptr until
    // enable_thread_correlation() opts this thread in. Only write_external_sample writes the slots (the gpu track); the
    // copier copies them in lockstep with samples under the one reader_busy hold, so they never desync. ATOMIC pointer
    // because enable_thread_correlation may publish it after `active` is already set, concurrently with a lock-free
    // reader/writer -- acquire/release on the pointer orders the array's allocation before its first use.
    std::atomic<CorrelationRecord*> correlation{nullptr}; // owning; size = per_thread_ring_slots when non-null
    std::atomic<crd::u64> head{0};  // monotonic writer cursor
    std::atomic<crd::u64> tail{0};  // monotonic reader cursor; cleared by clear_samples
    std::atomic<crd::u64> dropped{0};
    const char* name = nullptr;     // DIAG.6a(d): owned copy in the name arena (never the caller's pointer); nullptr = unnamed
    crd::u32 fiber_id_current = 0U; // set via set_current_fiber_id; consumed by push_region
    crd::u32 depth = 0U;            // nesting depth
    std::atomic<bool> active{false};      // true after register_thread publishes samples/head/tail (release/acquire)
    std::atomic<bool> reader_busy{false}; // (c3) exactly one consumer (copier or clearer) in the ring at a time
    std::atomic<crd::u64> contended{0};   // (c3) copy attempts that found the ring busy (exact; == contended returns)
    std::atomic<crd::u32> priority_waiters{0}; // (c3) saves waiting for this ring; other copiers step aside
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---- Interned name table -----------------------------------------------

struct NameEntry
{
    // DIAG.6a(d): points into the profiler's owned name arena (a copy), never the caller's buffer -- so it cannot
    // dangle. Atomic because resolve_name() reads it LOCK-FREE while intern_name() publishes it under names_mutex
    // (release on publish / acquire on read; the arena bytes are written before the pointer is stored).
    std::atomic<const char*> string{nullptr};
    crd::u32                 hash = 0U;
};

// ---- Counter table -----------------------------------------------------

struct CounterEntry
{
    // DIAG.6a(f): atomic like NameEntry::string / AllocatorEntry::name -- counter_info reads this LOCK-FREE while
    // register_counter_impl publishes it under counter_mutex. Published with a release store AFTER kind/type/bits and
    // read with an acquire load, so a caller resolving a CounterId it already holds (without going through the
    // counter_count acquire gate) still observes fully-written, arena-owned (never dangling) bytes. This closes DG08's
    // "unsynchronized registry reads" for the counter registry -- the last registry-name pointer (d) left non-atomic.
    std::atomic<const char*> name{nullptr};
    CounterKind              kind = CounterKind::Set;
    CounterType              type = CounterType::I64;
    std::atomic<crd::u64>    bits{0U}; // raw bits: i64 sign-extended / f64 bit_cast / i64 ns for Duration
};

// ---- Allocator table ---------------------------------------------------

// DIAG.6a(b): the slot's `allocator`/`name` are published atomically. Registration/unregistration happen under
// alloc_mutex; the snapshot readers (frame_mark, allocator_snapshot, allocator_info) never take that mutex -- they
// load the pointer once (seq_cst) and, for the deref paths, hold snapshot_in_flight so unregister waits for them.
// `generation` is bumped on every (re)registration into this slot so a later reader can detect slot reuse (6a(d)).
struct AllocatorEntry
{
    std::atomic<const char*>              name{nullptr};
    std::atomic<crd::memory::IAllocator*> allocator{nullptr};
    std::atomic<crd::u32>                 generation{0U};
    // DIAG.6a(d2): the interned NameId of `name` (0xFFFFFFFF = none). frame_mark stamps this per AllocatorRecord so a
    // saved frame keeps the name the slot had THEN, surviving unregister + slot reuse. Published before `allocator`.
    std::atomic<crd::u32>                 name_id{0xFFFF'FFFFU};
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

    // DIAG.6a(d): bounded byte arena owning every registry name (region/thread/counter/allocator). Allocated once at
    // init, freed at shutdown, never moved -- resolve_name pointers stay valid for the process lifetime (the table
    // never unregisters). `name_arena_used` is a lock-free bump cursor (own_name_bytes CAS-reserves), so the four
    // registry paths can copy without sharing a lock. Exhaustion is explicit via name_bytes_dropped, never a borrow.
    char*                 name_arena = nullptr;
    crd::u32              name_arena_bytes = 0U;
    std::atomic<crd::u32> name_arena_used{0U};
    std::atomic<crd::u64> name_bytes_dropped{0U};

    // Counter table (fixed-size, sequentially-indexed; protected by counter_mutex
    // on insert; lookup-by-name uses linear scan -- cold path, table is small).
    CounterEntry* counters = nullptr;            // size = kMaxCounters
    std::atomic<crd::u32> counter_count_atomic{0U}; // monotonically grows
    std::mutex counter_mutex;

    // Allocator table (fixed-size). Slots can be unregistered (allocator
    // pointer set to nullptr); the count never shrinks. Protected by
    // alloc_mutex on insert / unregister.
    AllocatorEntry* allocators = nullptr;          // size = kMaxAllocators
    std::atomic<crd::u32> allocator_count_atomic{0U}; // high-water of registered slots (never shrinks; stable index)
    std::atomic<crd::u32> allocator_live_count{0U};   // DIAG.6a(b): currently-registered count (rises + falls)
    std::atomic<crd::u32> snapshot_in_flight{0U};     // DIAG.6a(b): readers dereferencing allocator slots right now
    std::mutex alloc_mutex;

    // Rolling frame-history ring. Producer = frame_mark() (one thread); reader
    // can be the UI thread or capture thread.
    FrameRecord* frame_history = nullptr;         // size = frame_history_slots
    crd::u32     frame_history_slots = kFrameHistorySlots;
    std::atomic<crd::u64> frame_history_head{0U}; // total frames captured; index via head % slots
    // DIAG.6a(c1): a parallel per-slot seqlock so a cross-thread reader (copy_frame_record) never copies a record
    // frame_mark is mid-writing. Even = stable, odd = write in progress. The seq lives here, not in FrameRecord (that
    // is the pinned CPROF wire layout). frame_history_unavailable counts copies that lost the retry race (honest
    // "unavailable", per the acceptance -- not a spin).
    std::atomic<crd::u32>* frame_history_seq = nullptr; // size = frame_history_slots
    std::atomic<crd::u64>  frame_history_unavailable{0U};
    crd::i64     last_frame_end_ns = 0;           // bookkeeping for FrameRecord.frame_begin_ns

    // Per-thread ring slot count (configurable via InitConfig).
    crd::u32 per_thread_ring_slots = kPerThreadRingSlots;

    // Frame bookkeeping. UI consumers read these via current_frame_index().
    std::atomic<crd::u64> frame_count{0U};

    // DIAG.6a(e): unique per init() (see g_state_gen). A thread caches this in t_state_gen when it registers; a stale
    // TLS cache from a PRIOR profiler lifetime (shutdown on a DIFFERENT thread never cleared this thread's TLS) then
    // fails the generation match in register_thread and re-registers instead of dereferencing a freed/empty ring. Set
    // once before the state is published; never mutated, so a plain field read under a StateGuard is safe.
    crd::u32 generation = 0U;

    // Initialised flag.
    bool initialised = false;
};

// Singleton. Allocated on init(), retired (exchanged to nullptr) on shutdown().
// DIAG.6a(e): ATOMIC so a reader's load and shutdown's exchange form a well-defined seq_cst handshake (a plain pointer
// raced here). Reader entry points load it once into a local `ProfilerState* const g_state` shadow (see StateGuard);
// the per-sample hot path (push/pop) and per-write counter path load it relaxed and are covered instead by the
// existing "join every recording thread before shutdown" contract -- documented residuals, not guarded (an RMW there
// would defeat the zero-overhead gate).
std::atomic<ProfilerState*> g_state{nullptr};

// DIAG.6a(e): state-wide in-flight reader count. MUST live OUTSIDE ProfilerState: a reader increments this THEN loads
// g_state; if the count lived inside *g_state, shutdown could free *g_state between those two steps -- the very UAF
// this guards, one level up. shutdown() retires g_state then drains this to zero before delete.
std::atomic<crd::u32> g_in_flight{0U};

// DIAG.6a(e): monotonic profiler-lifetime counter. init() stamps (this + 1) into ProfilerState::generation, so every
// live state has a non-zero, distinct generation and the default t_state_gen == 0 never matches one.
std::atomic<crd::u32> g_state_gen{0U};

// Enter a state read: increment in-flight FIRST, then load g_state. If the profiler is inactive, back the increment out
// and return nullptr. The fetch_add and the load are BOTH seq_cst; paired with shutdown()'s seq_cst exchange-then-load
// this is a Dekker handshake -- at least one side observes the other's store, so a reader that began before the
// exchange is counted (shutdown waits for it) and one that begins after loads nullptr (backs out, touches nothing).
[[nodiscard]] ProfilerState* reader_enter() noexcept
{
    g_in_flight.fetch_add(1U, std::memory_order_seq_cst);
    ProfilerState* const s = g_state.load(std::memory_order_seq_cst);
    if (s == nullptr)
    {
        g_in_flight.fetch_sub(1U, std::memory_order_seq_cst);
    }
    return s;
}

void reader_leave() noexcept { g_in_flight.fetch_sub(1U, std::memory_order_seq_cst); }

// RAII pin for a single reader entry point. `s` is the loaded state (nullptr if inactive). Nested guards are fine (the
// count simply rises past 1) -- a guarded registration that calls intern_name/resolve_name re-enters harmlessly.
struct StateGuard
{
    ProfilerState* s;
    StateGuard() noexcept : s(reader_enter()) {}
    ~StateGuard() noexcept
    {
        if (s != nullptr)
        {
            reader_leave();
        }
    }
    StateGuard(const StateGuard&)            = delete;
    StateGuard& operator=(const StateGuard&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return s != nullptr; }
};

// Thread-local: the calling thread's index in g_state->rings, or kInvalidThread
// if not registered.
constexpr crd::u8 kInvalidThread = 0xFFU;
thread_local crd::u8 t_thread_index = kInvalidThread;

// Thread-local: cached pointer to the calling thread's ring. nullptr if the
// thread has not been registered or after shutdown.
thread_local ThreadRing* t_ring = nullptr;
// (c3) The ring this thread holds copy priority on (a save in its retry loop); kInvalidThread when none.
thread_local crd::u8 t_copy_priority_ring = kInvalidThread;

// DIAG.6a(e): the generation of the ProfilerState this thread's t_thread_index/t_ring cache belongs to. Compared
// against g_state->generation in register_thread so a cache left over from a previous init()/shutdown() cycle (on a
// thread shutdown never cleared) is detected and the thread re-registers against the new state. 0 = never registered.
thread_local crd::u32 t_state_gen = 0U;

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

// DIAG.6a(d): stable literal used when a registry name cannot be owned (arena/table exhausted or over-long). NEVER the
// caller's pointer -- borrowing it is exactly the dangle this slice removes.
constexpr const char* kNameStorageExhausted = "(name storage exhausted)";

// Copy `src` into the profiler's name arena and return a stable pointer, or nullptr if it cannot (src null, longer than
// kMaxNameBytes, or arena full), bumping name_bytes_dropped on a real drop. Lock-free (atomic CAS bump) so any of the
// four registry paths may call it without sharing a lock; the bytes are fully written before the caller publishes the
// returned pointer.
[[nodiscard]] const char* own_name_bytes(ProfilerState& st, const char* src) noexcept
{
    if (src == nullptr)
    {
        return nullptr;
    }
    const std::size_t len = std::strlen(src);
    if (len + 1U > kMaxNameBytes)
    {
        st.name_bytes_dropped.fetch_add(1U, std::memory_order_relaxed);
        return nullptr; // over-long: dropped, NOT truncated (truncation + content dedup could collide two names)
    }
    const crd::u32 need = static_cast<crd::u32>(len) + 1U;
    crd::u32       old  = st.name_arena_used.load(std::memory_order_relaxed);
    for (;;)
    {
        if (old > st.name_arena_bytes || st.name_arena_bytes - old < need)
        {
            st.name_bytes_dropped.fetch_add(1U, std::memory_order_relaxed);
            return nullptr; // arena exhausted
        }
        if (st.name_arena_used.compare_exchange_weak(old, old + need, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed))
        {
            break;
        }
    }
    char* const dst = st.name_arena + old;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

// For registry name fields (thread / counter / allocator): own the bytes, or fall back to the STABLE exhausted literal
// -- never the caller's pointer. nullptr in -> nullptr out (an explicitly unnamed entry).
[[nodiscard]] const char* own_registry_name(ProfilerState& st, const char* src) noexcept
{
    if (src == nullptr)
    {
        return nullptr;
    }
    const char* const owned = own_name_bytes(st, src);
    return owned != nullptr ? owned : kNameStorageExhausted;
}

// DIAG.6b(c): allocate + publish a fresh ring slot (the shared core of register_thread's fresh path and
// register_external_track). Bumps thread_count, initialises the ring, owns the name, and publishes `active` -- but
// touches NO thread-local state, so the caller decides whether the slot is its own track (register_thread binds TLS) or
// an external one (register_external_track does not). `assert_on_full` keeps register_thread's original hard assert on a
// kMaxThreads overflow; external producers pass false to degrade quietly. Returns kInvalidThread on overflow or OOM.
[[nodiscard]] crd::u8 allocate_ring_slot(ProfilerState& st, const char* name, bool assert_on_full) noexcept
{
    const crd::u32 idx = st.thread_count.fetch_add(1U, std::memory_order_acq_rel);
    if (idx >= kMaxThreads)
    {
        st.thread_count.fetch_sub(1U, std::memory_order_acq_rel);
        if (assert_on_full)
        {
            CRD_ASSERT_MSG(false, "crd-perf: kMaxThreads exceeded");
        }
        return kInvalidThread;
    }

    ThreadRing& ring = st.rings[idx];
    // `new (std::nothrow)`, not plain `new`: this is a `noexcept` registration path, so a throwing
    // bad_alloc here is std::terminate rather than a failed registration. A profiler that cannot get its
    // ring must degrade to "this thread records nothing", never take the process down.
    // (bugprone-unhandled-exception-at-new.)
    ring.samples = new (std::nothrow) Sample[st.per_thread_ring_slots]{};
    if (ring.samples == nullptr)
    {
        st.thread_count.fetch_sub(1U, std::memory_order_acq_rel);
        return kInvalidThread;
    }
    ring.head.store(0U, std::memory_order_relaxed);
    ring.tail.store(0U, std::memory_order_relaxed);
    ring.dropped.store(0U, std::memory_order_relaxed);
    ring.name             = own_registry_name(st, name); // DIAG.6a(d): own the bytes before publishing active
    ring.fiber_id_current = 0U;
    ring.depth            = 0U;
    ring.active.store(true, std::memory_order_release); // publish samples/head/tail before the ring is readable
    return static_cast<crd::u8>(idx);
}

} // namespace detail

// DIAG.6a(e): NO `using detail::g_state;` -- g_state is now atomic and must be reached deliberately. Every reader
// entry point declares a local `ProfilerState* const g_state` shadow (from a StateGuard); init/shutdown use the
// fully-qualified atomic directly. Any function that still names a bare `g_state` without a shadow fails to compile --
// a deliberate failsafe that no g_state-dereferencing path is left unguarded.
using detail::t_ring;
using detail::t_state_gen;
using detail::t_thread_index;

// ---- Init / shutdown ----------------------------------------------------

void init(const InitConfig& cfg)
{
    if (detail::g_state.load(std::memory_order_seq_cst) != nullptr)
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

    // DIAG.6a(d): the name arena that owns every registry name's bytes. Sized from all four capacities so realistic
    // (short) names never exhaust it before the tables themselves fill; over-budget names are dropped explicitly.
    state->name_arena_bytes =
        (state->names_capacity + kMaxThreads + kMaxCounters + kMaxAllocators) * kNameArenaBytesPerEntry;
    state->name_arena = new (std::nothrow) char[state->name_arena_bytes];
    if (state->name_arena == nullptr)
    {
        state->name_arena_bytes = 0U; // own_name_bytes then drops every name (explicit), never borrows
    }
    state->name_arena_used.store(0U, std::memory_order_relaxed);

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
    state->frame_history_seq = new std::atomic<crd::u32>[state->frame_history_slots]{}; // 0 = even = stable
    state->frame_history_head.store(0U, std::memory_order_relaxed);
    state->last_frame_end_ns = crd::time::MonotonicClock::now().ns_since_epoch();

    // Allocate per-thread rings lazily on register_thread(); leave them
    // nullptr here so unregistered slots are visibly inert.

    state->initialised = true;
    // DIAG.6a(e): stamp a unique, non-zero generation BEFORE publishing so register_thread can distinguish a TLS cache
    // from a prior lifetime (see ProfilerState::generation / t_state_gen).
    state->generation = detail::g_state_gen.fetch_add(1U, std::memory_order_relaxed) + 1U;
    // Publish (seq_cst) so a reader that enters after this observes the fully-built state; register_thread below then
    // takes its own StateGuard against the now-visible pointer.
    detail::g_state.store(state, std::memory_order_seq_cst);

    // Auto-register the calling thread as "main".
    (void)register_thread("main");
}

// Forward decl: lives in gpu_scope.cpp. Called from shutdown() to clear
// the cached GPU backend pointer + gpu thread index before the thread
// table is torn down.
namespace detail
{
void reset_gpu_state() noexcept;
// DIAG.6b(e): defined in gpu_scope.cpp; frame_mark() calls it to drive the GPU frame lifecycle (end -> resolve ->
// begin(next)). No-op without a backend.
void gpu_frame_advance(crd::u64 next_frame_index) noexcept;
} // namespace detail

void shutdown()
{
    // DIAG.6a(e): retire the state pointer FIRST (seq_cst exchange) so every reader that enters from now on loads
    // nullptr and touches nothing. Idempotent: a second shutdown exchanges nullptr for nullptr and returns.
    detail::ProfilerState* state = detail::g_state.exchange(nullptr, std::memory_order_seq_cst);
    if (state == nullptr)
    {
        return;
    }

    // Drain in-flight readers. Each incremented g_in_flight (seq_cst) BEFORE loading g_state (seq_cst); paired with the
    // seq_cst exchange above this is a Dekker handshake, so any reader that loaded the old pointer is counted here and
    // waited for, while any that arrives later loads nullptr. No mutex is held during the wait (a reader blocked on
    // alloc_mutex/names_mutex must be able to finish and decrement), so this cannot deadlock against a live reader.
    // Bounded by the longest in-flight read (<= capture's copy-retry budget). shutdown() must not be called from inside
    // a reader/StateReadGuard scope -- that would wait on itself.
    while (detail::g_in_flight.load(std::memory_order_seq_cst) != 0U)
    {
        std::this_thread::yield();
    }

    detail::reset_gpu_state();

    for (crd::u32 i = 0U; i < kMaxThreads; ++i)
    {
        delete[] state->rings[i].samples;
        state->rings[i].samples = nullptr;
        // DIAG.6b(b): free the optional slot-parallel array (nullptr if never enabled). Relaxed: shutdown has already
        // drained all in-flight readers (6a(e)), so no concurrent access remains.
        delete[] state->rings[i].correlation.load(std::memory_order_relaxed);
        state->rings[i].correlation.store(nullptr, std::memory_order_relaxed);
    }
    delete[] state->names;
    state->names = nullptr;
    delete[] state->name_arena; // DIAG.6a(d): frees every owned name; all resolve_name pointers die with the profiler
    state->name_arena = nullptr;
    delete[] state->counters;
    state->counters = nullptr;
    delete[] state->allocators;
    state->allocators = nullptr;
    delete[] state->frame_history;
    state->frame_history = nullptr;
    delete[] state->frame_history_seq;
    state->frame_history_seq = nullptr;
    delete state;

    // Clear thread-local caches in case the calling thread keeps running.
    t_ring = nullptr;
    t_thread_index = detail::kInvalidThread;
}

[[nodiscard]] bool is_active() noexcept
{
    return detail::g_state.load(std::memory_order_seq_cst) != nullptr;
}

// DIAG.6a(e): public multi-call read pin (declared in profiler.hpp). state_read_acquire pins state for the duration of
// a whole capture (see save_capture_to_buffer) so shutdown() blocks until it completes -- otherwise per-call guards
// would keep it crash-free but let a capture tear across a shutdown. A false return holds no pin.
namespace detail
{
[[nodiscard]] bool state_read_acquire() noexcept { return reader_enter() != nullptr; }
void               state_read_release() noexcept { reader_leave(); }
} // namespace detail

// ---- Name interning -----------------------------------------------------

[[nodiscard]] NameId intern_name(const char* name) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || name == nullptr)
    {
        return kInvalidNameId;
    }

    detail::ProfilerState& state = *g_state;
    const crd::u32         h     = detail::fnv1a(name);
    const crd::u32         mask  = state.names_capacity - 1U;

    std::lock_guard<std::mutex> lock(state.names_mutex);

    // Content-keyed (FNV-1a + strcmp), so a reused caller buffer holding different bytes hashes to a different slot --
    // there is no pointer-identity mislabel. The macro's TU-local cache means this cold path runs once per call site.
    crd::u32 idx = h & mask;
    for (crd::u32 step = 0U; step < state.names_capacity; ++step)
    {
        detail::NameEntry& e      = state.names[idx];
        const char* const  stored = e.string.load(std::memory_order_acquire);
        if (stored == nullptr)
        {
            // Free slot -- insert.
            if (state.names_count + 1U >= state.names_capacity)
            {
                // Table saturated -- explicit + non-fatal (bump kMaxRegionNames / InitConfig::max_region_names if it
                // recurs). Never borrow the caller's pointer as a fallback: that is the dangle (d) removes.
                state.name_bytes_dropped.fetch_add(1U, std::memory_order_relaxed);
                return kInvalidNameId;
            }
            // DIAG.6a(d): OWN the bytes. A name we cannot copy (arena full / over-long) is dropped (counted inside
            // own_name_bytes), never borrowed.
            const char* const owned = detail::own_name_bytes(state, name);
            if (owned == nullptr)
            {
                return kInvalidNameId;
            }
            e.hash = h;
            e.string.store(owned, std::memory_order_release); // publish AFTER the arena bytes + hash are written
            ++state.names_count;
            return NameId{idx};
        }
        if (e.hash == h && std::strcmp(stored, name) == 0)
        {
            return NameId{idx};
        }
        idx = (idx + 1U) & mask;
    }
    return kInvalidNameId;
}

[[nodiscard]] const char* resolve_name(NameId id) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || !id.is_valid() || id.value >= g_state->names_capacity)
    {
        return "";
    }
    // Lock-free acquire-load pairs with intern_name's release store (DIAG.6a(d)): the arena bytes were written before
    // the pointer was published, so a non-null result always points at fully-written, process-lifetime-stable bytes.
    const char* const s = g_state->names[id.value].string.load(std::memory_order_acquire);
    return s != nullptr ? s : "";
}

[[nodiscard]] crd::u32 intern_name_capacity() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->names_capacity : 0U;
}

[[nodiscard]] crd::u32 intern_name_count() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr)
    {
        return 0U;
    }
    // names_count is mutated under names_mutex; relaxed read is safe for
    // capture sizing (saturated upper bound). The reader will validate.
    std::lock_guard<std::mutex> lock(g_state->names_mutex);
    return g_state->names_count;
}

[[nodiscard]] crd::u64 name_bytes_dropped_count() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    // DIAG.6a(d): names refused because a table/arena was full or a name exceeded kMaxNameBytes. Non-zero means some
    // name is unavailable (kInvalidNameId / the "(name storage exhausted)" literal) -- explicit, never a silent borrow.
    return g_state != nullptr ? g_state->name_bytes_dropped.load(std::memory_order_relaxed) : 0U;
}

// ---- Thread registration ------------------------------------------------

crd::u8 register_thread(const char* name) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr)
    {
        return detail::kInvalidThread;
    }

    // Already registered IN THIS profiler lifetime? Refresh the t_ring cache and return. DIAG.6a(e): the generation
    // match is essential -- a TLS cache from a previous init()/shutdown() (shutdown clears TLS only for its own caller,
    // so a cross-thread shutdown leaves this thread's t_thread_index/t_ring stale) must NOT be reused; it points into
    // freed/empty rings. On a mismatch, fall through to a fresh registration against the new state.
    if (t_thread_index != detail::kInvalidThread && t_state_gen == g_state->generation)
    {
        t_ring = &g_state->rings[t_thread_index];
        // DIAG.6a(d): re-own only if the name actually changed. A thread re-registering under the SAME name each frame
        // (e.g. a worker pool) would otherwise leak a fresh arena copy per call. Cold-path strcmp guards it.
        if (name != nullptr && (t_ring->name == nullptr || std::strcmp(t_ring->name, name) != 0))
        {
            t_ring->name = detail::own_registry_name(*g_state, name);
        }
        return t_thread_index;
    }

    const crd::u8 idx = detail::allocate_ring_slot(*g_state, name, /*assert_on_full=*/true);
    if (idx == detail::kInvalidThread)
    {
        return detail::kInvalidThread;
    }
    t_thread_index = idx;
    t_ring         = &g_state->rings[idx];
    t_state_gen    = g_state->generation; // DIAG.6a(e): bind this TLS cache to the current profiler lifetime
    return t_thread_index;
}

// DIAG.6b(c): register a dedicated ring slot that is NOT the caller's own track and touches no thread-local state.
// Shares allocate_ring_slot with register_thread but degrades quietly on a full table (assert_on_full=false) so a
// many-track producer can fall back. No refresh/re-own path -- every call is a fresh slot; the caller owns the mapping.
crd::u8 register_external_track(const char* name) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr)
    {
        return detail::kInvalidThread;
    }
    return detail::allocate_ring_slot(*g_state, name, /*assert_on_full=*/false);
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
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
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

// ---- Frame-history seqlock payload --------------------------------------
//
// DIAG.6a(c1): a cross-thread reader may overlap frame_mark's write of a slot and then discards its copy. That overlap
// is defined behaviour only when both sides access the shared record atomically, so the record moves member by member
// through relaxed std::atomic_ref (a plain memcpy against the writer is a data race; TSan reported it 2026-10-05). The
// sequence counter still provides the ordering; same-thread readers of frame_record() keep plain access.
namespace
{
template <typename T>
void relaxed_store(T& dst, T value) noexcept
{
    static_assert(std::atomic_ref<T>::is_always_lock_free, "seqlock payload words must be lock-free");
    std::atomic_ref<T>(dst).store(value, std::memory_order_relaxed);
}

template <typename T>
[[nodiscard]] T relaxed_load(T& src) noexcept
{
    return std::atomic_ref<T>(src).load(std::memory_order_relaxed);
}

void move_allocator_record(AllocatorRecord& dst, AllocatorRecord& src, bool publish) noexcept
{
    if (publish)
    {
        relaxed_store(dst.alloc_count, src.alloc_count);
        relaxed_store(dst.dealloc_count, src.dealloc_count);
        relaxed_store(dst.bytes_in_use, src.bytes_in_use);
        relaxed_store(dst.peak_bytes, src.peak_bytes);
        relaxed_store(dst.total_bytes, src.total_bytes);
        relaxed_store(dst._pad, src._pad);
    }
    else
    {
        dst.alloc_count   = relaxed_load(src.alloc_count);
        dst.dealloc_count = relaxed_load(src.dealloc_count);
        dst.bytes_in_use  = relaxed_load(src.bytes_in_use);
        dst.peak_bytes    = relaxed_load(src.peak_bytes);
        dst.total_bytes   = relaxed_load(src.total_bytes);
        dst._pad          = relaxed_load(src._pad);
    }
}

// Publish the members frame_mark filled (the header, then the counted values and allocators) into the shared slot.
void publish_frame_record(FrameRecord& slot, FrameRecord& rec) noexcept
{
    relaxed_store(slot.frame_index, rec.frame_index);
    relaxed_store(slot.frame_begin_ns, rec.frame_begin_ns);
    relaxed_store(slot.frame_end_ns, rec.frame_end_ns);
    relaxed_store(slot.counter_count, rec.counter_count);
    relaxed_store(slot.allocator_count, rec.allocator_count);
    for (crd::u32 i = 0U; i < rec.counter_count; ++i)
    {
        relaxed_store(slot.values[i].bits, rec.values[i].bits);
    }
    for (crd::u32 i = 0U; i < rec.allocator_count; ++i)
    {
        move_allocator_record(slot.allocators[i], rec.allocators[i], true);
    }
}

// Copy a whole shared slot into `out` (as the former memcpy did); valid only if the sequence check then passes.
void read_frame_record(FrameRecord& out, FrameRecord& slot) noexcept
{
    out.frame_index     = relaxed_load(slot.frame_index);
    out.frame_begin_ns  = relaxed_load(slot.frame_begin_ns);
    out.frame_end_ns    = relaxed_load(slot.frame_end_ns);
    out.counter_count   = relaxed_load(slot.counter_count);
    out.allocator_count = relaxed_load(slot.allocator_count);
    for (crd::u32 i = 0U; i < kMaxCounters; ++i)
    {
        out.values[i].bits = relaxed_load(slot.values[i].bits);
    }
    for (crd::u32 i = 0U; i < kMaxAllocators; ++i)
    {
        move_allocator_record(out.allocators[i], slot.allocators[i], false);
    }
}
} // namespace

// ---- Frame boundary -----------------------------------------------------

void frame_mark() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
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
    // DIAG.6a(c1) seqlock: bump this slot's sequence to odd (write in progress) before touching the record; a
    // cross-thread copy_frame_record retries while it is odd or if it changes across the copy.
    std::atomic<crd::u32>& seq       = state.frame_history_seq[history_head_idx];
    const crd::u32         seq_begin = seq.load(std::memory_order_relaxed);
    seq.store(seq_begin + 1U, std::memory_order_relaxed); // odd
    std::atomic_thread_fence(std::memory_order_release);  // record writes below stay after the odd marker
    FrameRecord& slot     = state.frame_history[history_head_idx];
    FrameRecord  rec{};   // built locally, then published into the slot word by word (see publish_frame_record)
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
    // DIAG.6a(b): mark a snapshot in flight so a concurrent unregister_allocator waits before its owner destroys the
    // allocator; load each slot pointer exactly once (seq_cst) and deref only that local -- never re-read the slot.
    state.snapshot_in_flight.fetch_add(1U, std::memory_order_seq_cst);
    for (crd::u32 i = 0U; i < rec.allocator_count; ++i)
    {
        crd::memory::IAllocator* const alloc = state.allocators[i].allocator.load(std::memory_order_seq_cst);
        AllocatorRecord&               ar    = rec.allocators[i];
        if (alloc == nullptr)
        {
            ar = AllocatorRecord{};
            continue;
        }
        // DIAG.6a(d2): stamp the slot's interned name identity into this record as name_id + 1 (0 = unset). The
        // allocator pointer was loaded (seq_cst) above and is non-null, so its name_id -- published before the pointer
        // in register_allocator -- is visible. This is what preserves the label across slot reuse: an older frame keeps
        // the name the slot held at ITS frame_mark, not the current occupant's.
        const crd::u32 nid = state.allocators[i].name_id.load(std::memory_order_relaxed);
        const auto snap    = alloc->stats().snapshot();
        ar.alloc_count     = snap.alloc_count;
        ar.dealloc_count   = snap.dealloc_count;
        ar.bytes_in_use    = snap.bytes_in_use;
        ar.peak_bytes      = snap.peak_bytes;
        ar.total_bytes     = snap.total_bytes;
        ar._pad            = nid != 0xFFFF'FFFFU ? static_cast<crd::u64>(nid) + 1U : 0U;
    }
    state.snapshot_in_flight.fetch_sub(1U, std::memory_order_seq_cst);

    // DIAG.6a(c1) seqlock: copy the record into the slot, then publish it by returning the sequence to even (release).
    publish_frame_record(slot, rec);
    seq.store(seq_begin + 2U, std::memory_order_release);

    // Retire the slot -- readers see this frame's record once the head
    // advances past it (release ordering on the head store).
    state.frame_history_head.fetch_add(1U, std::memory_order_acq_rel);
    state.last_frame_end_ns = end_ns;

    // DIAG.6b(e): drive the GPU frame lifecycle -- close the frame that just ended, resolve completed GPU frames, and
    // open the next (index = the count after this mark's increment). No-op without a backend. Runs under this frame_mark's
    // StateGuard, so shutdown's drain waits for it.
    detail::gpu_frame_advance(frame_index + 1U);
}

[[nodiscard]] crd::u64 frame_count() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->frame_count.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] crd::u64 current_frame_index() noexcept { return frame_count(); }

// ---- Introspection -----------------------------------------------------

[[nodiscard]] ThreadSamplesView thread_samples(crd::u8 thread_index) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    ThreadSamplesView view{nullptr, 0U, 0U, nullptr};
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return view;
    }
    detail::ThreadRing& ring = g_state->rings[thread_index];
    if (!ring.active.load(std::memory_order_acquire))
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
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->thread_count.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] crd::u32 copy_thread_samples(crd::u8 thread_index, Sample* out, crd::u32 max_samples,
                                           bool* out_contended) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (out_contended != nullptr)
    {
        *out_contended = false;
    }
    if (g_state == nullptr || out == nullptr || max_samples == 0U || thread_index >= kMaxThreads)
    {
        return 0U;
    }
    detail::ThreadRing& ring = g_state->rings[thread_index];
    if (!ring.active.load(std::memory_order_acquire)) // acquire: samples/head/tail published before active
    {
        return 0U;
    }
    // (c3) Enforced single consumer: exchange-acquire the ring, or report contention. A busy ring returns 0 AND sets
    // *out_contended (distinguishable from a genuinely empty ring, which returns 0 with out_contended false) and bumps
    // `contended` -- exactly once per refused attempt, so the counter equals the number of contended returns.
    // A save waiting for this ring has priority: every other copier refuses exactly as if the ring were busy.
    const bool yield_to_save =
        detail::t_copy_priority_ring != thread_index && ring.priority_waiters.load(std::memory_order_acquire) != 0U;
    if (yield_to_save || ring.reader_busy.exchange(true, std::memory_order_acquire))
    {
        ring.contended.fetch_add(1U, std::memory_order_relaxed);
        if (out_contended != nullptr)
        {
            *out_contended = true;
        }
        return 0U;
    }
    const crd::u64 h     = ring.head.load(std::memory_order_acquire);
    const crd::u64 t     = ring.tail.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->per_thread_ring_slots;
    crd::u64       count = h - t;
    if (count > slots)
    {
        count = slots; // the ring holds at most `slots` live samples
    }
    if (count > max_samples)
    {
        count = max_samples;
    }
    // Oldest-first, wrap-correct: the live samples are at [t & mask, h & mask), NOT contiguous from the base. The
    // reader_busy flag (above) plus the drop-on-full producer (which only ever touches [head, tail+slots)) make this
    // copy tear-free without a seqlock -- no second consumer and no tail-mover (clear_samples) can run against it.
    const crd::u32 mask = static_cast<crd::u32>(slots - 1U);
    for (crd::u64 i = 0U; i < count; ++i)
    {
        out[i] = ring.samples[static_cast<crd::u32>(t + i) & mask];
    }
    ring.reader_busy.store(false, std::memory_order_release);
    return static_cast<crd::u32>(count);
}

// DIAG.6b(b): opt a thread into per-slot correlation -- allocate the slot-parallel CorrelationRecord array. Cold path
// (call once, at registration): lazily allocates, idempotent, and safe against a racing enable (CAS install; the loser
// frees its array). A nothrow failure leaves correlation disabled rather than terminating this noexcept path.
void enable_thread_correlation(crd::u8 thread_index) noexcept
{
    detail::StateGuard           state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return;
    }
    detail::ThreadRing& ring = g_state->rings[thread_index];
    if (ring.correlation.load(std::memory_order_acquire) != nullptr)
    {
        return; // already enabled
    }
    auto* const arr = new (std::nothrow) CorrelationRecord[g_state->per_thread_ring_slots]{};
    if (arr == nullptr)
    {
        return;
    }
    CorrelationRecord* expected = nullptr;
    if (!ring.correlation.compare_exchange_strong(expected, arr, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
    {
        delete[] arr; // lost the race to another enable; keep the installed array
    }
}

[[nodiscard]] bool thread_has_correlation(crd::u8 thread_index) noexcept
{
    detail::StateGuard           state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return false;
    }
    return g_state->rings[thread_index].correlation.load(std::memory_order_acquire) != nullptr;
}

// DIAG.6b(b): like copy_thread_samples but ALSO copies each sample's slot-parallel CorrelationRecord into corr_out under
// the SAME reader_busy hold -- so out[i] and corr_out[i] describe the same sample, wrap-safe, no key. A thread without a
// correlation array yields cleared records (all-zero = kValid clear). corr_out must hold at least max_samples records.
[[nodiscard]] crd::u32 copy_thread_samples_with_correlation(crd::u8 thread_index, Sample* out,
                                                            CorrelationRecord* corr_out, crd::u32 max_samples,
                                                            bool* out_contended) noexcept
{
    detail::StateGuard           state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (out_contended != nullptr)
    {
        *out_contended = false;
    }
    if (g_state == nullptr || out == nullptr || corr_out == nullptr || max_samples == 0U
        || thread_index >= kMaxThreads)
    {
        return 0U;
    }
    detail::ThreadRing& ring = g_state->rings[thread_index];
    if (!ring.active.load(std::memory_order_acquire))
    {
        return 0U;
    }
    // A save waiting for this ring has priority: every other copier refuses exactly as if the ring were busy.
    const bool yield_to_save =
        detail::t_copy_priority_ring != thread_index && ring.priority_waiters.load(std::memory_order_acquire) != 0U;
    if (yield_to_save || ring.reader_busy.exchange(true, std::memory_order_acquire))
    {
        ring.contended.fetch_add(1U, std::memory_order_relaxed);
        if (out_contended != nullptr)
        {
            *out_contended = true;
        }
        return 0U;
    }
    const crd::u64 h     = ring.head.load(std::memory_order_acquire);
    const crd::u64 t     = ring.tail.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->per_thread_ring_slots;
    crd::u64       count = h - t;
    if (count > slots)
    {
        count = slots;
    }
    if (count > max_samples)
    {
        count = max_samples;
    }
    const crd::u32                 mask     = static_cast<crd::u32>(slots - 1U);
    const CorrelationRecord* const corr_arr = ring.correlation.load(std::memory_order_acquire);
    for (crd::u64 i = 0U; i < count; ++i)
    {
        const crd::u32 slot = static_cast<crd::u32>(t + i) & mask;
        out[i]              = ring.samples[slot];
        corr_out[i]         = corr_arr != nullptr ? corr_arr[slot] : CorrelationRecord{};
    }
    ring.reader_busy.store(false, std::memory_order_release);
    return static_cast<crd::u32>(count);
}

// (c3) Number of copy_thread_samples attempts that found thread `thread_index`'s ring held by another consumer. Exact:
// equals the count of contended (out_contended == true) returns. Monotonic within an init()/shutdown() lifetime.
void sample_copy_priority_begin(crd::u8 thread_index) noexcept
{
    detail::StateGuard           state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return;
    }
    g_state->rings[thread_index].priority_waiters.fetch_add(1U, std::memory_order_acq_rel);
    detail::t_copy_priority_ring = thread_index;
}

void sample_copy_priority_end(crd::u8 thread_index) noexcept
{
    detail::StateGuard           state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || thread_index >= kMaxThreads || detail::t_copy_priority_ring != thread_index)
    {
        return;
    }
    detail::t_copy_priority_ring = detail::kInvalidThread;
    g_state->rings[thread_index].priority_waiters.fetch_sub(1U, std::memory_order_acq_rel);
}

[[nodiscard]] crd::u64 sample_copy_contended_count(crd::u8 thread_index) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || thread_index >= kMaxThreads)
    {
        return 0U;
    }
    return g_state->rings[thread_index].contended.load(std::memory_order_relaxed);
}

[[nodiscard]] crd::u32 per_thread_ring_capacity() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->per_thread_ring_slots : 0U;
}

void clear_samples() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr)
    {
        return;
    }
    for (crd::u32 i = 0U; i < kMaxThreads; ++i)
    {
        detail::ThreadRing& ring = g_state->rings[i];
        if (!ring.active.load(std::memory_order_acquire))
        {
            continue;
        }
        // (c3) clear is a consumer too: it moves `tail`, which would break a concurrent copier's disjointness. Take the
        // same single-consumer flag first. Clear is rare (UI/capture transition), and the copier window it waits on is
        // one bounded (<= slots) memcpy, so a plain acquire spin is fine -- no other consumer holds it for longer.
        while (ring.reader_busy.exchange(true, std::memory_order_acquire))
        {
            // busy-wait: a copier is mid-copy on this ring; it releases after a bounded memcpy.
        }
        const crd::u64 h = ring.head.load(std::memory_order_acquire);
        ring.tail.store(h, std::memory_order_release);
        ring.dropped.store(0U, std::memory_order_relaxed);
        ring.reader_busy.store(false, std::memory_order_release);
    }
}

// ---- External sample write (backend-internal) --------------------------
//
// Used by the GPU backend (a gpu-context backend, DIAG.6b(i); today only the test mock) to push a
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

void write_external_sample(crd::u8 thread_index, const Sample& s, const CorrelationRecord* corr) noexcept
{
    detail::ProfilerState* const state_ptr = detail::g_state.load(std::memory_order_relaxed);
    if (state_ptr == nullptr || thread_index >= kMaxThreads)
    {
        return;
    }
    ThreadRing& ring = state_ptr->rings[thread_index];
    if (!ring.active.load(std::memory_order_acquire))
    {
        return;
    }
    const crd::u32 mask  = state_ptr->per_thread_ring_slots - 1U;
    const crd::u64 h     = ring.head.load(std::memory_order_relaxed);
    const crd::u64 t     = ring.tail.load(std::memory_order_acquire);
    const crd::u64 slots = state_ptr->per_thread_ring_slots;
    if (h - t >= slots)
    {
        ring.dropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    ring.samples[h & mask] = s;
    // DIAG.6b(b): write the slot's correlation record BEFORE publishing head. If this thread opted in, ALWAYS write the
    // slot (the given record, or a cleared one) so a reused slot never shows a stale record -- "all-zero = none" is the
    // clear. slot-parallel: same index as the sample, so the copier joins them by ordinal.
    CorrelationRecord* const corr_arr = ring.correlation.load(std::memory_order_acquire);
    if (corr_arr != nullptr)
    {
        corr_arr[h & mask] = corr != nullptr ? *corr : CorrelationRecord{};
    }
    ring.head.store(h + 1U, std::memory_order_release);
}

} // namespace detail

// ---- Counter registration (cold path) -----------------------------------

namespace detail
{

[[nodiscard]] CounterId register_counter_impl(const char* static_name, CounterKind kind,
                                              CounterType type) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const state_ptr = state_guard.s;
    if (state_ptr == nullptr || static_name == nullptr)
    {
        return kInvalidCounterId;
    }
    detail::ProfilerState& state = *state_ptr;
    std::lock_guard<std::mutex> lock(state.counter_mutex);

    const crd::u32 n = state.counter_count_atomic.load(std::memory_order_relaxed);
    // Dedup by name + kind + type. Three-key dedup keeps "draws" / Set / I64
    // distinct from "draws" / Add / I64 -- a user mistake we surface
    // by giving them different ids rather than aliasing.
    for (crd::u32 i = 0U; i < n; ++i)
    {
        const detail::CounterEntry& e   = state.counters[i];
        const char* const           enm = e.name.load(std::memory_order_relaxed); // under counter_mutex; published slot
        if (enm == nullptr)
        {
            continue;
        }
        if (e.kind == kind && e.type == type && std::strcmp(enm, static_name) == 0)
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
    // DIAG.6a(f): write kind/type/bits, THEN publish name with a release store -- a lock-free counter_info that
    // acquire-loads a non-null name is then guaranteed to see kind/type too. DIAG.6a(d): own the bytes; never borrow.
    slot.kind = kind;
    slot.type = type;
    slot.bits.store(0U, std::memory_order_relaxed);
    slot.name.store(detail::own_registry_name(state, static_name), std::memory_order_release);
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
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    g_state->counters[id.value].bits.store(static_cast<crd::u64>(value), std::memory_order_relaxed);
}

void counter_set_f64(CounterId id, crd::f64 value) noexcept
{
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    g_state->counters[id.value].bits.store(std::bit_cast<crd::u64>(value), std::memory_order_relaxed);
}

void counter_set_duration(CounterId id, crd::time::Duration value) noexcept
{
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    const crd::i64 ns = static_cast<crd::i64>(value.value * 1.0e9);
    g_state->counters[id.value].bits.store(static_cast<crd::u64>(ns), std::memory_order_relaxed);
}

void counter_add_i64(CounterId id, crd::i64 delta) noexcept
{
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return;
    }
    g_state->counters[id.value].bits.fetch_add(static_cast<crd::u64>(delta),
                                               std::memory_order_relaxed);
}

void counter_add_f64(CounterId id, crd::f64 delta) noexcept
{
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
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
    detail::ProfilerState* const g_state = detail::g_state.load(std::memory_order_relaxed);
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
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->counter_count_atomic.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] CounterInfo counter_info(CounterId id) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    CounterInfo info{"", CounterKind::Set, CounterType::I64};
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return info;
    }
    const detail::CounterEntry& e  = g_state->counters[id.value];
    // DIAG.6a(f): acquire-load pairs with register_counter_impl's release store, so a non-null name points at
    // fully-written arena bytes (never a dangle, never a torn read) and kind/type written before it are visible.
    const char* const           nm = e.name.load(std::memory_order_acquire);
    info.name = nm != nullptr ? nm : "";
    info.kind = e.kind;
    info.type = e.type;
    return info;
}

[[nodiscard]] crd::i64 counter_current_i64(CounterId id) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return 0;
    }
    return static_cast<crd::i64>(g_state->counters[id.value].bits.load(std::memory_order_relaxed));
}

[[nodiscard]] crd::f64 counter_current_f64(CounterId id) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return 0.0;
    }
    return std::bit_cast<crd::f64>(g_state->counters[id.value].bits.load(std::memory_order_relaxed));
}

[[nodiscard]] crd::time::Duration counter_current_duration(CounterId id) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || !id.is_valid() || id.value >= kMaxCounters)
    {
        return crd::time::Duration{};
    }
    const crd::i64 ns = static_cast<crd::i64>(
        g_state->counters[id.value].bits.load(std::memory_order_relaxed));
    return crd::time::Duration{static_cast<crd::f64>(ns) * 1.0e-9};
}

// ---- Allocator registry (cold path) ------------------------------------

namespace
{
// DIAG.6a(d2): resolve a registry name to (owned pointer, interned NameId). Interning DEDUPS (no arena leak on repeat)
// and yields the id frame_mark stamps into each AllocatorRecord so history survives slot reuse. On name-table
// saturation the id is kInvalidNameId and the pointer falls back to a fresh arena copy (or the exhausted literal).
struct OwnedName
{
    const char* ptr;
    crd::u32    id;
};
[[nodiscard]] OwnedName intern_registry_name(detail::ProfilerState& st, const char* name) noexcept
{
    const NameId nid = intern_name(name);
    if (nid.is_valid())
    {
        return {resolve_name(nid), nid.value};
    }
    return {detail::own_registry_name(st, name), nid.value}; // nid.value == kInvalidNameId
}
} // namespace

[[nodiscard]] crd::u32 register_allocator(const char* name,
                                          crd::memory::IAllocator* allocator) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr || name == nullptr || allocator == nullptr)
    {
        return kInvalidAllocatorIdx;
    }
    detail::ProfilerState& state = *g_state;
    std::lock_guard<std::mutex> lock(state.alloc_mutex);

    const crd::u32 n = state.allocator_count_atomic.load(std::memory_order_relaxed);

    // Dedup by allocator pointer; same instance always returns the same idx. Relabel only -- not a new live slot.
    for (crd::u32 i = 0U; i < n; ++i)
    {
        if (state.allocators[i].allocator.load(std::memory_order_relaxed) == allocator)
        {
            // DIAG.6a(d): only own new bytes if the name actually CHANGED. The arena is bump-only, so re-owning on every
            // relabel with the same name (e.g. a component re-registered per level load) would leak arena bytes and
            // eventually starve region names. A cold-path strcmp avoids that.
            const char* const cur = state.allocators[i].name.load(std::memory_order_relaxed);
            if (cur == nullptr || std::strcmp(cur, name) != 0)
            {
                const OwnedName on = intern_registry_name(state, name);
                state.allocators[i].name_id.store(on.id, std::memory_order_seq_cst);
                state.allocators[i].name.store(on.ptr, std::memory_order_seq_cst);
            }
            return i;
        }
    }
    // A genuinely new slot -- own+intern the name once here (never on the dedup path above).
    const OwnedName on = intern_registry_name(state, name);
    // Publish `name` + `name_id` before `allocator`: the allocator pointer is the gate a reader tests, so once it is
    // visible the name identity is already set. `generation` bumps on every (re)use of a slot so a reader can detect
    // slot reuse (6a(d)). Re-use a cleared slot (unregister leaves a hole) before appending.
    for (crd::u32 i = 0U; i < n; ++i)
    {
        if (state.allocators[i].allocator.load(std::memory_order_relaxed) == nullptr)
        {
            state.allocators[i].name.store(on.ptr, std::memory_order_seq_cst);
            state.allocators[i].name_id.store(on.id, std::memory_order_seq_cst);
            state.allocators[i].generation.fetch_add(1U, std::memory_order_relaxed);
            state.allocators[i].allocator.store(allocator, std::memory_order_seq_cst);
            state.allocator_live_count.fetch_add(1U, std::memory_order_seq_cst);
            return i;
        }
    }
    if (n >= kMaxAllocators)
    {
        CRD_ASSERT_MSG(false, "crd-perf: kMaxAllocators exceeded");
        return kInvalidAllocatorIdx;
    }
    state.allocators[n].name.store(on.ptr, std::memory_order_seq_cst);
    state.allocators[n].name_id.store(on.id, std::memory_order_seq_cst);
    state.allocators[n].generation.fetch_add(1U, std::memory_order_relaxed);
    state.allocators[n].allocator.store(allocator, std::memory_order_seq_cst);
    state.allocator_live_count.fetch_add(1U, std::memory_order_seq_cst);
    state.allocator_count_atomic.store(n + 1U, std::memory_order_release);
    return n;
}

void unregister_allocator(crd::u32 allocator_idx) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
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
    // Only a live slot decrements the live count (idempotent unregister of an already-cleared slot is a no-op).
    if (state.allocators[allocator_idx].allocator.load(std::memory_order_relaxed) != nullptr)
    {
        state.allocator_live_count.fetch_sub(1U, std::memory_order_seq_cst);
    }
    // Retire the slot, then WAIT for any snapshot that may already hold this slot's old pointer to finish. The
    // in_flight RMWs and this store are seq_cst so the store->load cannot reorder past the wait (a Dekker-shaped
    // hazard): a snapshot that began before the store is drained here; one that begins after loads null. Only after
    // this returns may the caller destroy the allocator. Do NOT call unregister_allocator from inside
    // IAllocator::stats() -- the snapshot holds the guard and this would spin forever.
    state.allocators[allocator_idx].name.store(nullptr, std::memory_order_seq_cst);
    state.allocators[allocator_idx].name_id.store(0xFFFF'FFFFU, std::memory_order_seq_cst); // (d2): clear stale identity
    state.allocators[allocator_idx].allocator.store(nullptr, std::memory_order_seq_cst);
    while (state.snapshot_in_flight.load(std::memory_order_seq_cst) != 0U)
    {
        std::this_thread::yield();
    }
    // High-water (allocator_count_atomic) stays unchanged -- slots become re-usable but the count never shrinks so the
    // UI keeps a stable index; live_allocator_count tracks how many are registered right now.
}

[[nodiscard]] crd::u32 registered_allocator_count() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->allocator_count_atomic.load(std::memory_order_acquire) : 0U;
}

[[nodiscard]] crd::u32 live_allocator_count() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->allocator_live_count.load(std::memory_order_seq_cst) : 0U;
}

[[nodiscard]] AllocatorInfo allocator_info(crd::u32 allocator_idx) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    AllocatorInfo info{};
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return info;
    }
    if (allocator_idx >= g_state->allocator_count_atomic.load(std::memory_order_acquire))
    {
        return info;
    }
    // Metadata only -- no deref, so no in-flight guard. Load atomically to avoid a torn read racing register/unregister.
    // The returned raw pointer follows the same owner contract: destroy only after unregister_allocator returns.
    const detail::AllocatorEntry& e = g_state->allocators[allocator_idx];
    const char* const             nm = e.name.load(std::memory_order_seq_cst);
    info.name      = nm != nullptr ? nm : "";
    info.allocator = e.allocator.load(std::memory_order_seq_cst);
    return info;
}

[[nodiscard]] AllocatorSnapshot allocator_snapshot(crd::u32 allocator_idx) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    AllocatorSnapshot snap{};
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return snap;
    }
    if (allocator_idx >= g_state->allocator_count_atomic.load(std::memory_order_acquire))
    {
        return snap;
    }
    // DIAG.6a(b): hold snapshot_in_flight across the deref so a concurrent unregister waits before its owner destroys
    // the allocator; load the slot pointer once (seq_cst) and deref only that local.
    detail::ProfilerState& state = *g_state;
    state.snapshot_in_flight.fetch_add(1U, std::memory_order_seq_cst);
    crd::memory::IAllocator* const alloc = state.allocators[allocator_idx].allocator.load(std::memory_order_seq_cst);
    if (alloc == nullptr)
    {
        state.snapshot_in_flight.fetch_sub(1U, std::memory_order_seq_cst);
        return snap;
    }
    const auto        raw = alloc->stats().snapshot();
    const char* const nm  = state.allocators[allocator_idx].name.load(std::memory_order_seq_cst);
    state.snapshot_in_flight.fetch_sub(1U, std::memory_order_seq_cst);
    snap.name           = nm != nullptr ? nm : "";
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
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    AllocatorSnapshot snap{};
    if (g_state == nullptr || allocator_idx >= kMaxAllocators)
    {
        return snap;
    }
    // DIAG.6a(c1): copy the historical record under the seqlock (this may be called cross-thread) rather than
    // dereferencing a raw pointer into the ring.
    FrameRecord hist{};
    if (!copy_frame_record(frames_back, hist) || allocator_idx >= hist.allocator_count)
    {
        return snap;
    }
    if (allocator_idx >= g_state->allocator_count_atomic.load(std::memory_order_acquire))
    {
        return snap;
    }
    const detail::AllocatorEntry& e  = g_state->allocators[allocator_idx];
    const AllocatorRecord&        ar = hist.allocators[allocator_idx];
    const char* const             nm = e.name.load(std::memory_order_seq_cst); // stats come from the copied history rec
    snap.name           = nm != nullptr ? nm : "";
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
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
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
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr)
    {
        return 0U;
    }
    const crd::u64 head  = g_state->frame_history_head.load(std::memory_order_acquire);
    const crd::u64 slots = g_state->frame_history_slots;
    return static_cast<crd::u32>(head < slots ? head : slots);
}

[[nodiscard]] bool copy_frame_record(crd::u32 frames_back, FrameRecord& out) noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    if (g_state == nullptr)
    {
        return false;
    }
    detail::ProfilerState& state = *g_state;
    const crd::u64         head  = state.frame_history_head.load(std::memory_order_acquire);
    if (head == 0U || frames_back >= state.frame_history_slots || frames_back >= head)
    {
        return false;
    }
    const crd::u64        target = head - 1U - static_cast<crd::u64>(frames_back);
    const crd::u32        idx    = static_cast<crd::u32>(target % state.frame_history_slots);
    std::atomic<crd::u32>& seq   = state.frame_history_seq[idx];
    // Seqlock read: the relaxed copy is speculative and DISCARDED unless the sequence was even and unchanged
    // across it (the design's "documented synchronization scheme"). Bounded retry -- a slot being lapped faster than we
    // can copy is reported as unavailable, never an unbounded spin (the acceptance's "unavailable" count).
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const crd::u32 s1 = seq.load(std::memory_order_acquire);
        if ((s1 & 1U) != 0U)
        {
            continue; // a write is in progress
        }
        read_frame_record(out, state.frame_history[idx]);
        std::atomic_thread_fence(std::memory_order_acquire); // the copy completes before we re-read the sequence
        const crd::u32 s2 = seq.load(std::memory_order_relaxed);
        if (s1 == s2)
        {
            return true; // stable: no writer touched this slot during the copy
        }
    }
    state.frame_history_unavailable.fetch_add(1U, std::memory_order_relaxed);
    return false;
}

[[nodiscard]] crd::u64 frame_history_unavailable_count() noexcept
{
    detail::StateGuard state_guard;
    detail::ProfilerState* const g_state = state_guard.s;
    return g_state != nullptr ? g_state->frame_history_unavailable.load(std::memory_order_relaxed) : 0U;
}

#else // CRD_PERF_ENABLED == 0

// All stubs below the gate. Identical signatures so consumer headers and
// the unit tests compile in either configuration.

void init(const InitConfig&) {}
void shutdown() {}
[[nodiscard]] bool is_active() noexcept { return false; }

namespace detail
{
[[nodiscard]] bool state_read_acquire() noexcept { return false; } // DIAG.6a(e): no state when profiling is compiled out
void               state_read_release() noexcept {}
} // namespace detail

[[nodiscard]] NameId intern_name(const char*) noexcept { return kInvalidNameId; }
[[nodiscard]] const char* resolve_name(NameId) noexcept { return ""; }
[[nodiscard]] crd::u32 intern_name_capacity() noexcept { return 0U; }
[[nodiscard]] crd::u32 intern_name_count() noexcept { return 0U; }
[[nodiscard]] crd::u64 name_bytes_dropped_count() noexcept { return 0U; }

crd::u8  register_thread(const char*) noexcept { return 0xFFU; }
crd::u8  register_external_track(const char*) noexcept { return 0xFFU; }
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
[[nodiscard]] crd::u32 copy_thread_samples(crd::u8, Sample*, crd::u32, bool* out_contended) noexcept
{
    if (out_contended != nullptr)
    {
        *out_contended = false; // compiled out: every ring is genuinely empty, never contended
    }
    return 0U;
}
[[nodiscard]] crd::u64 sample_copy_contended_count(crd::u8) noexcept { return 0U; }
void                    sample_copy_priority_begin(crd::u8) noexcept {}
void                    sample_copy_priority_end(crd::u8) noexcept {}
[[nodiscard]] crd::u32 per_thread_ring_capacity() noexcept { return 0U; } // "0 when the profiler is inactive"
void enable_thread_correlation(crd::u8) noexcept {} // DIAG.6b(b)
[[nodiscard]] bool thread_has_correlation(crd::u8) noexcept { return false; }
[[nodiscard]] crd::u32 copy_thread_samples_with_correlation(crd::u8, Sample*, CorrelationRecord*, crd::u32,
                                                            bool* out_contended) noexcept
{
    if (out_contended != nullptr)
    {
        *out_contended = false;
    }
    return 0U;
}
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
[[nodiscard]] bool copy_frame_record(crd::u32, FrameRecord&) noexcept { return false; }
[[nodiscard]] crd::u64 frame_history_unavailable_count() noexcept { return 0U; }

[[nodiscard]] crd::u32 register_allocator(const char*, crd::memory::IAllocator*) noexcept
{
    return kInvalidAllocatorIdx;
}
void unregister_allocator(crd::u32) noexcept {}
[[nodiscard]] crd::u32 registered_allocator_count() noexcept { return 0U; }
[[nodiscard]] crd::u32 live_allocator_count() noexcept { return 0U; }
[[nodiscard]] AllocatorInfo allocator_info(crd::u32) noexcept { return AllocatorInfo{}; }
[[nodiscard]] AllocatorSnapshot allocator_snapshot(crd::u32) noexcept { return AllocatorSnapshot{}; }
[[nodiscard]] AllocatorSnapshot allocator_snapshot_history(crd::u32, crd::u32) noexcept
{
    return AllocatorSnapshot{};
}

#endif // CRD_PERF_ENABLED

} // namespace crd::perf
