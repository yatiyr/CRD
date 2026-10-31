// crd-perf -- triggerable bounded flight recording (DIAG.9b). See flight_recorder.hpp.

#include <crd/perf/flight_recorder.hpp>

#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>
#include <crd/perf/hardware_identity.hpp>

#include <crd/core/platform.hpp>
#include <crd/memory/asan_poison.hpp>

#include "json_writer.hpp"
#include "payload_codec.hpp"
#include "striped_count.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace crd::perf
{
namespace
{
using detail::append_json_escaped;
using detail::append_u64;
using detail::Cursor;
using detail::put_bytes;
using detail::put_u16;
using detail::put_u32;
using detail::put_u64;
using detail::put_u8;

// Armed -> Triggered (trigger(), under the trigger mutex) -> Complete (the claim of the post window's last sequence,
// or a collector) -> Armed (collection, under both mutexes). A capture that ended Fatal moves to FatalEntered instead of
// re-arming: the process is ending, and a later fatal path (check_invariant's default reaches CRD_FATAL, which reaches
// the assert trigger) must not replace the capture that was already delivered. Triggers are then suppressed and events
// dropped; nothing more is delivered.
constexpr crd::u32 kStateArmed        = 0U;
constexpr crd::u32 kStateTriggered    = 1U;
constexpr crd::u32 kStateComplete     = 2U;
constexpr crd::u32 kStateFatalEntered = 3U;

// The recording word: the total above kStateBits; below it the state and, apart from it, the reloading flag, set
// while a policy reload swaps the rings (claims wait). A flag, not a state, so a fatal path's move to FatalEntered and
// the reload's end each keep the other's bits. A claim adds kClaimOne.
constexpr crd::u32 kStateBits    = 3U;
constexpr crd::u64 kStateMask    = 3U;
constexpr crd::u64 kReloadingBit = 4U;
constexpr crd::u64 kClaimOne     = crd::u64{1} << kStateBits;

// An atomic u64 that holds no total yet (the frozen window's end, a reload's base).
constexpr crd::u64 kNoTotal = ~crd::u64{0};

// A shared-ring slot no claim has published since its ring was allocated, and one a claim is writing.
constexpr crd::u64 kSlotEmpty = ~crd::u64{0};
constexpr crd::u64 kSlotBusy  = kSlotEmpty - 1U;
static_assert(alignof(FlightEvent) >= std::atomic_ref<crd::u64>::required_alignment,
              "a shared slot's sequence is its publication stamp");

constexpr cont::StringView kNoRecipe{"no replay recipe was registered with the flight recorder"};

// The fatal path waits a bounded number of yields, shared by the collection lock and the claims still publishing: it
// must not block forever behind a thread that will never go on.
constexpr crd::u32 kFatalWaitAttempts = 2000U;

// Copy `src` into a fixed buffer of `cap` bytes, clipped to cap - 1 and NUL-terminated. Returns the stored length.
crd::u16 copy_clipped(char* dst, crd::u32 cap, cont::StringView src, bool* truncated) noexcept
{
    const crd::usize limit = static_cast<crd::usize>(cap) - 1U;
    const crd::usize n     = (src.size() < limit) ? src.size() : limit;
    if (n > 0U)
    {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
    if (truncated != nullptr)
    {
        *truncated = src.size() > limit;
    }
    return static_cast<crd::u16>(n);
}

// Copy a source path into a fixed buffer of `cap` bytes. A path longer than cap - 1 keeps its tail, which names the
// file, after a "..." marker (the head is the least telling part of an absolute path); the cut never splits a UTF-8
// sequence. Returns the stored length.
crd::u16 copy_path_tail(char* dst, crd::u32 cap, cont::StringView src) noexcept
{
    const crd::usize limit = static_cast<crd::usize>(cap) - 1U;
    if (src.size() <= limit)
    {
        return copy_clipped(dst, cap, src, nullptr);
    }
    constexpr crd::usize marker = 3U;
    crd::usize           from   = src.size() - (limit - marker);
    while (from < src.size() && (static_cast<crd::u8>(src[from]) & 0xC0U) == 0x80U)
    {
        ++from;
    }
    const crd::usize n = src.size() - from;
    std::memcpy(dst, "...", marker);
    if (n > 0U)
    {
        std::memcpy(dst + marker, src.data() + from, n);
    }
    dst[marker + n] = '\0';
    return static_cast<crd::u16>(marker + n);
}

bool known_trigger(crd::u32 kind) noexcept
{
    return kind > 0U && kind < kFlightTriggerCount;
}

constexpr crd::u8 kFlightMaxDetail = static_cast<crd::u8>(FlightDetail::Basic);
} // namespace

cont::StringView flight_trigger_name(FlightTrigger t) noexcept
{
    switch (t)
    {
    case FlightTrigger::None:              return cont::StringView{"none"};
    case FlightTrigger::Manual:            return cont::StringView{"manual"};
    case FlightTrigger::Assertion:         return cont::StringView{"assertion"};
    case FlightTrigger::FatalInvariant:    return cont::StringView{"fatal-invariant"};
    case FlightTrigger::AllocationFailure: return cont::StringView{"allocation-failure"};
    case FlightTrigger::FrameHitch:        return cont::StringView{"frame-hitch"};
    case FlightTrigger::QueueStarvation:   return cont::StringView{"queue-starvation"};
    case FlightTrigger::Hang:              return cont::StringView{"hang"};
    case FlightTrigger::ResourcePressure:  return cont::StringView{"resource-pressure"};
    case FlightTrigger::InjectedFault:     return cont::StringView{"injected-fault"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView flight_end_name(FlightEnd e) noexcept
{
    switch (e)
    {
    case FlightEnd::None:              return cont::StringView{"open"};
    case FlightEnd::PostEventsFull:    return cont::StringView{"post-events-full"};
    case FlightEnd::PostWindowElapsed: return cont::StringView{"post-window-elapsed"};
    case FlightEnd::Shutdown:          return cont::StringView{"shutdown"};
    case FlightEnd::Fatal:             return cont::StringView{"fatal"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView flight_detail_name(FlightDetail d) noexcept
{
    switch (d)
    {
    case FlightDetail::Full:  return cont::StringView{"full"};
    case FlightDetail::Basic: return cont::StringView{"basic"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView flight_policy_status_name(FlightPolicyStatus s) noexcept
{
    switch (s)
    {
    case FlightPolicyStatus::Committed:              return cont::StringView{"committed"};
    case FlightPolicyStatus::RejectedUnknownVersion: return cont::StringView{"rejected-unknown-version"};
    case FlightPolicyStatus::RejectedOutOfBounds:    return cont::StringView{"rejected-out-of-bounds"};
    case FlightPolicyStatus::RejectedBusy:           return cont::StringView{"rejected-busy"};
    case FlightPolicyStatus::RejectedNotInitialized: return cont::StringView{"rejected-not-initialized"};
    case FlightPolicyStatus::RejectedOutOfMemory:    return cont::StringView{"rejected-out-of-memory"};
    case FlightPolicyStatus::RejectedGenerationWrap: return cont::StringView{"rejected-generation-wrap"};
    }
    return cont::StringView{"unknown"};
}

FlightPolicyStatus validate_flight_policy(const FlightPolicy& p) noexcept
{
    if (p.schema_version != kFlightPolicyVersion)
    {
        return FlightPolicyStatus::RejectedUnknownVersion;
    }
    if (p.capacity < 2U || p.capacity > kFlightMaxEvents || p.pre_events == 0U || p.post_events == 0U ||
        p.pre_events > p.capacity || p.post_events > p.capacity - p.pre_events ||
        p.post_window_ns > kFlightMaxPostWindowNs || p.hitch_threshold_ns == 0U || p.trigger_mask == 0U ||
        (p.trigger_mask & ~kFlightAllTriggers) != 0U || static_cast<crd::u8>(p.detail) > kFlightMaxDetail)
    {
        return FlightPolicyStatus::RejectedOutOfBounds;
    }
    return FlightPolicyStatus::Committed;
}

namespace
{
// One thread's ring (DIAG.11b (q1)). Only the thread whose token is in `owner` writes `claims`, `head`, `next` and the
// slots; a collector reads them after an acquire load of `claims` that finds no claim in flight, while recording is
// frozen. `claims` is odd while the owner is inside a claim: it is marked before the claim's fetch_add, so a collector
// that saw the frozen word also sees every earlier claim's mark. The storage changes only in a policy reload, with
// every claim waiting; a thread keeps its ring across reloads and gives it back when it exits.
struct alignas(64) ThreadRing
{
    std::atomic<crd::u64> owner{0U};  // the bound thread's token; 0: free
    std::atomic<crd::u64> claims{0U}; // the owner's claims, twice each: odd while one is in flight
    crd::u64              head     = 0U;      // events written since the storage was allocated
    crd::u32              next     = 0U;      // the slot the next event takes
    crd::u32              capacity = 0U;      // slots, or 0 without storage (the thread records into the shared ring)
    FlightEvent*          events   = nullptr; // `capacity` slots; slot i is constructed when head first passes it
    char                  pad[24]  = {};      // the rest of the line, so no other ring's owner writes it
};
static_assert(sizeof(ThreadRing) == 64U, "a thread ring's header is one cache line");

// The storage of one policy: flight_thread_rings(capacity) rings of `capacity` slots, each starting on a cache line,
// then the shared ring's `capacity` slots, in one block from the default allocator.
struct RingStorage
{
    void*        block       = nullptr;
    crd::usize   bytes       = 0U;
    crd::usize   ring_stride = 0U; // bytes from one thread ring's storage to the next
    crd::u32     rings       = 0U;
    crd::u32     capacity    = 0U;
    FlightEvent* shared      = nullptr;
};
} // namespace

// MSVC C4324: structure padded due to alignment specifier. Expected: the recording word's and the read-mostly fields'
// cache lines are deliberate (the profiler's and crd-jobs' ThreadState's suppression).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct FlightRecorder::Impl
{
    // The recording state is one word: the total recorded above kStateBits, the state below. record() claims a
    // sequence with one fetch_add of it; the state it returns decides whether the claim is kept, so the trigger's
    // boundary is exact in claim order and the order agrees with happens-before. The record path takes no lock
    // (DIAG.11b). Alone on its line: every kept event writes it.
    alignas(64) std::atomic<crd::u64> word{kStateArmed};

    // Read by every event: the recorder's identity (the threads' ring bindings name it) and the lowest severity the
    // active detail keeps, as its u8 value. A claim that finds Triggered reads post_end: where the open post window
    // ends (the trigger's sequence plus post_events). trigger() writes it before its exchange publishes Triggered.
    alignas(64) crd::u64  id = 0U;
    std::atomic<crd::u8>  keep_from{0U};
    std::atomic<crd::u64> post_end{0U};
    // The sequence the frozen window ends at, once frozen (kNoTotal while armed or triggered): post_end when the claim
    // of its last sequence closed it, else the total at a collector's close. Kept while FatalEntered.
    std::atomic<crd::u64> frozen_end{kNoTotal};
    // A reload's base while it runs (kNoTotal otherwise), for total_recorded().
    std::atomic<crd::u64> reload_base{kNoTotal};
    // Bumped when a thread gives its ring back or a reload changes the storage, so a thread bound to the shared ring
    // looks for a ring again.
    std::atomic<crd::u64> release_epoch{0U};
    // The thread rings with storage (a binding prefers them), the claims through the shared ring by threads that hold
    // no ring (a reload waits for them) and the events kept in the shared ring.
    std::atomic<crd::u32> storage_rings{0U};
    std::atomic<crd::u64> shared_inflight{0U};
    std::atomic<crd::u64> shared_events{0U};
    // The events the detail left out, striped: one counter was a line every thread wrote at every left-out site, beside
    // `keep_from`, which every site reads (DIAG.11b: 1.2 points of a four-thread jobs frame on linux-gcc-relwithdebinfo).
    detail::StripedCount filtered;

    ThreadRing  rings[kFlightThreadRings];
    RingStorage storage;
    crd::u64    base = 0U; // the total at the storage's allocation: no capture reaches below it

    // Collection: poll(), take_capture(), finish() and reload_policy() hold collect_mutex; record() never takes it.
    // `end` is how a collector closed the window; None means the claim of its last sequence closed it.
    std::mutex collect_mutex;
    FlightEnd  end    = FlightEnd::None;
    crd::u64   end_ns = 0U;

    // The trigger and the replay recipe: trigger_mutex. The trigger record is written only while Armed, before the
    // exchange that publishes Triggered, so a thread that observes Triggered or Complete reads it safely.
    std::mutex          trigger_mutex;
    FlightTriggerRecord trigger;
    char                trigger_replay[kFlightRecipeBytes] = {};
    crd::u16            trigger_replay_length               = 0U;
    bool                trigger_replay_available            = false;
    char                replay[kFlightRecipeBytes]          = {};
    crd::u16            replay_length                       = 0U;
    bool                replay_available                    = false;
    crd::u64            suppressed                          = 0U;
    crd::u32            suppressed_kinds                    = 0U;

    // The policy is written under both mutexes with no claim in flight, so a holder of either reads it safely.
    FlightPolicy policy;
    crd::u32     policy_generation = 1U;

    std::atomic<FlightCaptureSink> sink{nullptr};
    std::atomic<void*>             sink_user{nullptr};
    std::atomic<crd::u64>          delivered{0U};

    Impl* next_live = nullptr; // the live-recorder list (the thread-exit release walks it)
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace
{
using Impl = FlightRecorder::Impl;

crd::u32 state_of(crd::u64 word) noexcept
{
    return static_cast<crd::u32>(word & kStateMask);
}

crd::u64 total_of(crd::u64 word) noexcept
{
    return word >> kStateBits;
}

crd::u64 make_word(crd::u64 total, crd::u32 state) noexcept
{
    return (total << kStateBits) | state;
}

crd::u64 lesser(crd::u64 a, crd::u64 b) noexcept
{
    return (a < b) ? a : b;
}

// The lowest severity `detail` keeps: Full keeps every event, Basic Warning and above.
crd::u8 keep_from_of(FlightDetail detail) noexcept
{
    const Severity lowest = (detail == FlightDetail::Basic) ? Severity::Warning : Severity::Info;
    return static_cast<crd::u8>(lowest);
}

// One yield of a bounded wait: false once `budget` (nullptr: unbounded) ran out.
bool spend(crd::u32* budget) noexcept
{
    if (budget != nullptr)
    {
        if (*budget == 0U)
        {
            return false;
        }
        --*budget;
    }
    std::this_thread::yield();
    return true;
}

// ---- Ring storage --------------------------------------------------------------------------------------------------

#if CRD_ENABLE_ASSERTS
std::atomic<crd::u32> g_refuse_storage{0U}; // FlightRecorder::test_refuse_ring_storage

bool storage_refused() noexcept
{
    crd::u32 left = g_refuse_storage.load(std::memory_order_relaxed);
    while (left > 0U)
    {
        if (g_refuse_storage.compare_exchange_weak(left, left - 1U, std::memory_order_relaxed))
        {
            return true;
        }
    }
    return false;
}
#endif

bool allocate_storage(RingStorage& out, crd::u32 capacity) noexcept
{
#if CRD_ENABLE_ASSERTS
    if (storage_refused())
    {
        return false;
    }
#endif
    const crd::u32   rings  = flight_thread_rings(capacity);
    const crd::usize slots  = static_cast<crd::usize>(capacity) * sizeof(FlightEvent);
    const crd::usize stride = (slots + 63U) & ~crd::usize{63U};
    const crd::usize bytes  = stride * rings + slots;
    void* const      block  = crd::memory::default_allocator()->try_allocate(bytes, 64U);
    if (block == nullptr)
    {
        return false;
    }
    out.block       = block;
    out.bytes       = bytes;
    out.ring_stride = stride;
    out.rings       = rings;
    out.capacity    = capacity;
    out.shared      = reinterpret_cast<FlightEvent*>(static_cast<char*>(block) + stride * rings);
    // A thread ring's slot is constructed when its ring first reaches it and read only after; until then the slots stay
    // poisoned, so a collector reading one it never wrote is reported. The shared ring's stamps are read before a
    // slot's first write, so its slots are constructed empty now.
    crd::memory::asan_poison(block, stride * rings);
    for (crd::u32 i = 0U; i < capacity; ++i)
    {
        FlightEvent* const slot = new (&out.shared[i]) FlightEvent{};
        slot->sequence          = kSlotEmpty;
    }
    return true;
}

void free_storage(RingStorage& s) noexcept
{
    if (s.block != nullptr)
    {
        crd::memory::asan_unpoison(s.block, s.bytes); // the allocator gets back exactly what it handed out
        crd::memory::default_allocator()->deallocate(s.block);
    }
    s = RingStorage{};
}

// With every claim waiting (init, or a reload with every ring idle): hand `s` to the rings, which start empty.
void install_storage(Impl& impl, const RingStorage& s) noexcept
{
    impl.storage = s;
    for (crd::u32 i = 0U; i < kFlightThreadRings; ++i)
    {
        ThreadRing& r = impl.rings[i];
        r.head        = 0U;
        r.next        = 0U;
        r.capacity    = (i < s.rings) ? s.capacity : 0U;
        r.events      = (i < s.rings) ? reinterpret_cast<FlightEvent*>(static_cast<char*>(s.block) + s.ring_stride * i)
                                      : nullptr;
    }
    impl.storage_rings.store(s.rings, std::memory_order_relaxed);
}

// ---- Thread binding and lifetime -----------------------------------------------------------------------------------

std::atomic<crd::u64> g_next_recorder{1U}; // recorder ids (0 names none)
std::atomic<crd::u64> g_next_token{1U};    // thread tokens (0 names no thread); never an OS thread id, which is reused

// The live recorders, for the thread-exit release: a mutex that is never destroyed and an intrusive list, so a thread
// that exits after static destruction still finds them. A blocking lock, not a spinlock: many threads exiting at once
// spun on one, and under ThreadSanitizer the holder's release starved behind the spinners' exchanges. A recorder leaves
// the list before it is freed, so the release never touches one that shut down.
Impl* g_live = nullptr;

std::mutex& live_mutex() noexcept
{
    alignas(std::mutex) static unsigned char storage[sizeof(std::mutex)];
    static std::mutex* const kLive = new (storage) std::mutex(); // never destroyed (above)
    return *kLive;
}

void lock_live() noexcept
{
    live_mutex().lock();
}

void unlock_live() noexcept
{
    live_mutex().unlock();
}

void add_live(Impl* impl) noexcept
{
    lock_live();
    impl->next_live = g_live;
    g_live          = impl;
    unlock_live();
}

void remove_live(Impl* impl) noexcept
{
    lock_live();
    for (Impl** at = &g_live; *at != nullptr; at = &(*at)->next_live)
    {
        if (*at == impl)
        {
            *at = impl->next_live;
            break;
        }
    }
    unlock_live();
}

// A thread that exits gives back every ring it holds in a live recorder; the ring's events stay for the captures that
// cover them, and the next thread to take the ring continues after them.
void release_thread_rings(crd::u64 token) noexcept
{
    if (token == 0U)
    {
        return;
    }
    lock_live();
    for (Impl* impl = g_live; impl != nullptr; impl = impl->next_live)
    {
        bool released = false;
        for (ThreadRing& r : impl->rings)
        {
            crd::u64 expected = token;
            released = r.owner.compare_exchange_strong(expected, 0U, std::memory_order_release,
                                                       std::memory_order_relaxed) || released;
        }
        if (released)
        {
            impl->release_epoch.fetch_add(1U, std::memory_order_release);
        }
    }
    unlock_live();
}

// A thread's ring in the recorder it last recorded into. Trivially constructible thread-local state: reading it needs
// no initialisation guard. `ring` null is the shared ring, looked for again once the recorder's release epoch moves.
struct FlightBinding
{
    crd::u64    recorder = 0U;
    ThreadRing* ring     = nullptr;
    crd::u64    epoch    = 0U;
};

thread_local FlightBinding t_binding;
thread_local crd::u64      t_token      = 0U;
thread_local bool          t_exit_armed = false;

// The thread-exit hook, armed when a thread first binds. Windows: a thread-local object's destructor (the CRT keeps
// the first 30 of a thread's in thread-local storage, without allocating). Elsewhere: a pthread key's destructor
// (glibc keeps a low key's value in the thread descriptor, without allocating; its thread_local destructor list
// allocates, and aborts when that fails). A thread whose hook could not be armed keeps its rings until the recorder
// shuts down.
#if defined(_WIN32)
struct ThreadExitRelease
{
    bool armed = true;
    ThreadExitRelease() noexcept = default;
    ~ThreadExitRelease() { release_thread_rings(t_token); }
    ThreadExitRelease(const ThreadExitRelease&)            = delete;
    ThreadExitRelease& operator=(const ThreadExitRelease&) = delete;
};

thread_local ThreadExitRelease t_exit_release;

void arm_thread_exit() noexcept
{
    t_exit_armed = t_exit_release.armed;
}
#else
void release_on_thread_exit(void* value)
{
    release_thread_rings(static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(value)));
}

struct ExitKey
{
    pthread_key_t key{};
    bool          ok = false;
};

ExitKey make_exit_key() noexcept
{
    ExitKey k;
    k.ok = pthread_key_create(&k.key, &release_on_thread_exit) == 0;
    return k;
}

void arm_thread_exit() noexcept
{
    static const ExitKey key = make_exit_key();
    t_exit_armed             = true;
    if (key.ok)
    {
        (void)pthread_setspecific(key.key, reinterpret_cast<void*>(static_cast<std::uintptr_t>(t_token)));
    }
}
#endif

// The slow path of a thread's first event in a recorder: its own ring if it still holds one, else a free ring (one
// with storage first; one without still gives the thread its own claim mark, its events going to the shared ring),
// else the shared ring. No allocation and no lock: a scan of the ring headers and one compare-exchange.
ThreadRing* bind_thread(Impl& impl) noexcept
{
    if (t_token == 0U)
    {
        t_token = g_next_token.fetch_add(1U, std::memory_order_relaxed);
    }
    if (!t_exit_armed)
    {
        arm_thread_exit();
    }
    const crd::u64 token = t_token;
    const crd::u64 epoch = impl.release_epoch.load(std::memory_order_acquire);
    ThreadRing*    found = nullptr;
    for (ThreadRing& r : impl.rings)
    {
        if (r.owner.load(std::memory_order_acquire) == token)
        {
            found = &r;
            break;
        }
    }
    const crd::u32 with_storage = impl.storage_rings.load(std::memory_order_relaxed); // a preference only
    for (crd::u32 pass = 0U; found == nullptr && pass < 2U; ++pass)
    {
        const crd::u32 from = (pass == 0U) ? 0U : with_storage;
        const crd::u32 to   = (pass == 0U) ? with_storage : kFlightThreadRings;
        for (crd::u32 i = from; i < to && found == nullptr; ++i)
        {
            ThreadRing& r        = impl.rings[i];
            crd::u64    expected = 0U;
            if (r.owner.load(std::memory_order_relaxed) == 0U &&
                r.owner.compare_exchange_strong(expected, token, std::memory_order_acquire, std::memory_order_relaxed))
            {
                found = &r;
            }
        }
    }
    t_binding = FlightBinding{impl.id, found, epoch};
    return found;
}

// The calling thread's ring in `impl` (nullptr: the shared ring).
ThreadRing* thread_ring(Impl& impl) noexcept
{
    const FlightBinding& b = t_binding;
    if (b.recorder == impl.id &&
        (b.ring != nullptr || b.epoch == impl.release_epoch.load(std::memory_order_relaxed)))
    {
        return b.ring;
    }
    return bind_thread(impl);
}

// ---- Claims ---------------------------------------------------------------------------------------------------------

struct Claim
{
    crd::u64 seq  = 0U;
    crd::u64 mark = 0U; // the ring's odd claim mark
    bool     kept = false;
};

void wait_reload(const Impl& impl) noexcept
{
    while ((impl.word.load(std::memory_order_acquire) & kReloadingBit) != 0U)
    {
        std::this_thread::yield();
    }
}

// The claim of the post window's last sequence closes the window: its end is post_end whoever moves the state (a
// collector that closes it at the same time read a total past it), and every later claim is void.
void close_by_claim(Impl& impl, crd::u64 end) noexcept
{
    impl.frozen_end.store(end, std::memory_order_relaxed);
    crd::u64 word = impl.word.load(std::memory_order_acquire);
    while (state_of(word) == kStateTriggered)
    {
        if (impl.word.compare_exchange_weak(word, make_word(total_of(word), kStateComplete), std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        {
            return;
        }
    }
}

// Claim the next sequence. The ring is marked busy first, so a collector or reload that saw the word after this
// claim's fetch_add sees the mark. A claim that finds a reload under way waits for it and claims again; one that finds
// the window frozen or past its end is void (kept false) and writes nothing.
Claim begin_claim(Impl& impl, ThreadRing* ring) noexcept
{
    for (;;)
    {
        Claim c;
        if (ring != nullptr)
        {
            c.mark = ring->claims.load(std::memory_order_relaxed) + 1U;
            ring->claims.store(c.mark, std::memory_order_relaxed);
        }
        else
        {
            impl.shared_inflight.fetch_add(1U, std::memory_order_acq_rel);
        }
        const crd::u64 word = impl.word.fetch_add(kClaimOne, std::memory_order_acq_rel);
        if ((word & kReloadingBit) != 0U)
        {
            if (ring != nullptr)
            {
                ring->claims.store(c.mark + 1U, std::memory_order_release);
            }
            else
            {
                impl.shared_inflight.fetch_sub(1U, std::memory_order_release);
            }
            wait_reload(impl);
            continue;
        }
        c.seq                = total_of(word);
        const crd::u32 state = state_of(word);
        if (state == kStateArmed)
        {
            c.kept = true;
        }
        else if (state == kStateTriggered)
        {
            const crd::u64 end = impl.post_end.load(std::memory_order_relaxed);
            c.kept             = c.seq < end;
            if (c.seq + 1U == end)
            {
                close_by_claim(impl, end);
            }
        }
        return c;
    }
}

void end_claim(Impl& impl, ThreadRing* ring, const Claim& c) noexcept
{
    if (ring != nullptr)
    {
        ring->claims.store(c.mark + 1U, std::memory_order_release);
    }
    else
    {
        impl.shared_inflight.fetch_sub(1U, std::memory_order_release);
    }
}

void fill(FlightEvent& slot, crd::u64 now, EventCode code, Severity severity, cont::StringView message,
          crd::u64 value_a, crd::u64 value_b, RedactionClass redaction) noexcept
{
    slot.timestamp_ns = now;
    slot.value_a      = value_a;
    slot.value_b      = value_b;
    slot.code         = code;
    slot.severity     = severity;
    slot.redaction    = redaction;
    // A restricted event's message is never copied, so no route out of the ring can carry it.
    const cont::StringView kept = redaction == RedactionClass::Restricted ? cont::StringView{} : message;
    slot.message_length =
        static_cast<crd::u8>(copy_clipped(slot.message, kFlightMessageBytes, kept, &slot.message_truncated));
}

// A kept claim's event into the shared ring's slot of its sequence. The window holds at most `capacity` consecutive
// sequences, so its events have distinct slots; a slot that already holds a later sequence (one `capacity` or more
// after this one) means this event is outside every window, and is not written.
void publish_shared(Impl& impl, crd::u64 seq, crd::u64 now, EventCode code, Severity severity,
                    cont::StringView message, crd::u64 value_a, crd::u64 value_b, RedactionClass redaction) noexcept
{
    FlightEvent&                    slot = impl.storage.shared[seq % impl.storage.capacity];
    const std::atomic_ref<crd::u64> stamp(slot.sequence);
    crd::u64                        seen = stamp.load(std::memory_order_acquire);
    for (;;)
    {
        if (seen == kSlotBusy)
        {
            std::this_thread::yield();
            seen = stamp.load(std::memory_order_acquire);
            continue;
        }
        if (seen != kSlotEmpty && seen > seq)
        {
            return;
        }
        if (stamp.compare_exchange_weak(seen, kSlotBusy, std::memory_order_acquire, std::memory_order_acquire))
        {
            break;
        }
    }
    fill(slot, now, code, severity, message, value_a, value_b, redaction);
    stamp.store(seq, std::memory_order_release);
    impl.shared_events.fetch_add(1U, std::memory_order_relaxed);
}

// A kept claim's event into the thread's ring, or the shared ring when the thread has none with storage.
void publish(Impl& impl, ThreadRing* ring, crd::u64 seq, crd::u64 now, EventCode code, Severity severity,
             cont::StringView message, crd::u64 value_a, crd::u64 value_b,
             RedactionClass redaction = RedactionClass::Public) noexcept
{
    if (ring == nullptr || ring->events == nullptr)
    {
        publish_shared(impl, seq, now, code, severity, message, value_a, value_b, redaction);
        return;
    }
    FlightEvent* slot = &ring->events[ring->next];
    if (ring->head < ring->capacity)
    {
        crd::memory::asan_unpoison(slot, sizeof(FlightEvent));
        slot = new (slot) FlightEvent{};
    }
    fill(*slot, now, code, severity, message, value_a, value_b, redaction);
    slot->sequence = seq;
    ring->next     = (ring->next + 1U == ring->capacity) ? 0U : ring->next + 1U;
    ++ring->head;
}

// ---- Collection -----------------------------------------------------------------------------------------------------

// Wait until no ring has a claim in flight that it had when called. Recording is frozen, so a claim that starts later
// is void. `budget` bounds the fatal path; returns false when it ran out.
bool wait_rings_idle(Impl& impl, crd::u32* budget) noexcept
{
    return std::ranges::all_of(impl.rings,
                               [budget](const ThreadRing& r)
                               {
                                   const crd::u64 mark = r.claims.load(std::memory_order_acquire);
                                   if ((mark & 1U) == 0U)
                                   {
                                       return true;
                                   }
                                   while (r.claims.load(std::memory_order_acquire) == mark)
                                   {
                                       if (!spend(budget))
                                       {
                                           return false;
                                       }
                                   }
                                   return true;
                               });
}

// The events of sequences [first, end) in sequence order: each ring's newest events back to `first`, placed at their
// sequence's position, then the shared ring's for the positions left (waiting for a claim still writing one). Every
// sequence below the total is a kept event and nothing is written after the window froze, so every position is filled.
// Returns false when `budget` ran out.
bool gather(Impl& impl, crd::u64 first, crd::u64 end, cont::Array<FlightEvent>& out, crd::u32* budget) noexcept
{
    out.clear();
    out.resize(static_cast<crd::usize>(end - first));
    for (FlightEvent& e : out)
    {
        e.sequence = kSlotEmpty;
    }
    for (const ThreadRing& r : impl.rings)
    {
        if (r.events == nullptr)
        {
            continue;
        }
        const crd::u64 held = lesser(r.head, r.capacity);
        crd::u32       slot = r.next;
        for (crd::u64 i = 0U; i < held; ++i)
        {
            slot                 = (slot == 0U) ? r.capacity - 1U : slot - 1U;
            const FlightEvent& e = r.events[slot];
            if (e.sequence < first)
            {
                break; // a ring's sequences rise from its oldest event to its newest
            }
            if (e.sequence < end)
            {
                out[static_cast<crd::usize>(e.sequence - first)] = e;
            }
        }
    }
    for (crd::usize p = 0U; p < out.size(); ++p)
    {
        if (out[p].sequence != kSlotEmpty)
        {
            continue;
        }
        const crd::u64                  s    = first + p;
        FlightEvent&                    slot = impl.storage.shared[s % impl.storage.capacity];
        const std::atomic_ref<crd::u64> stamp(slot.sequence);
        while (stamp.load(std::memory_order_acquire) != s)
        {
            if (!spend(budget))
            {
                return false;
            }
        }
        out[p] = slot;
    }
    return true;
}

// With the trigger mutex held: the capture of the trigger alone, said so (the fatal path could not read the rings).
// Terminal, as after any Fatal capture.
void trigger_only_capture(Impl& impl, FlightCapture& out, crd::u64 now) noexcept
{
    out.trigger = impl.trigger;
    out.end = FlightEnd::Fatal;
    out.end_ns = now;
    out.policy = impl.policy;
    out.policy_generation = impl.policy_generation;
    out.recorded_before_trigger = impl.trigger.sequence;
    out.suppressed_triggers = impl.suppressed;
    out.suppressed_kinds = impl.suppressed_kinds;
    out.replay_available = impl.trigger_replay_available;
    out.replay.clear();
    out.replay.append(cont::StringView{impl.trigger_replay, impl.trigger_replay_length});
    out.events.clear();
    out.pre_retained = 0U;
    out.post_retained = 0U;
    out.dropped_while_complete = 0U;
    out.ring_unavailable = true;
    out.post_truncated = true;
    // From Triggered (1) or Complete (2) to FatalEntered (3); record() and the collectors leave that state alone. From
    // Triggered the window freezes where the total was (or at post_end, past which claims were already void).
    const crd::u64 word = impl.word.fetch_or(kStateFatalEntered, std::memory_order_acq_rel);
    if (state_of(word) == kStateTriggered)
    {
        impl.frozen_end.store(lesser(total_of(word), impl.post_end.load(std::memory_order_relaxed)),
                              std::memory_order_release);
    }
}

// With the collect mutex held: when the state is Complete, build `out` and re-arm. `budget` bounds the wait for
// claims still publishing (the fatal path); when it runs out, `out` is the trigger alone. Returns false when nothing
// is complete.
bool build_capture(Impl& impl, FlightCapture& out, crd::u32* budget) noexcept
{
    if (state_of(impl.word.load(std::memory_order_acquire)) != kStateComplete)
    {
        return false;
    }

    std::lock_guard<std::mutex> trigger_guard(impl.trigger_mutex);
    const crd::u64 end   = impl.frozen_end.load(std::memory_order_acquire); // no claim moves it while Complete
    const crd::u64 trig  = impl.trigger.sequence;
    const crd::u64 want  = (trig > impl.policy.pre_events) ? trig - impl.policy.pre_events : 0U;
    const crd::u64 first = (want > impl.base) ? want : impl.base;
    if (!wait_rings_idle(impl, budget) || !gather(impl, first, end, out.events, budget))
    {
        trigger_only_capture(impl, out, diagnostic_now_ns());
        return true;
    }

    out.trigger                 = impl.trigger;
    out.end                     = impl.end;
    out.end_ns                  = impl.end_ns;
    out.policy                  = impl.policy;
    out.policy_generation       = impl.policy_generation;
    out.suppressed_triggers     = impl.suppressed;
    out.suppressed_kinds        = impl.suppressed_kinds;
    out.replay_available        = impl.trigger_replay_available;
    out.replay.clear();
    out.replay.append(cont::StringView{impl.trigger_replay, impl.trigger_replay_length});
    out.recorded_before_trigger = trig;
    out.pre_retained            = static_cast<crd::u32>(trig - first);
    out.post_retained           = static_cast<crd::u32>(end - trig);
    out.ring_unavailable        = false;
    if (out.end == FlightEnd::None) // the claim of the window's last sequence closed it, at its own time
    {
        out.end = FlightEnd::PostEventsFull;
        out.end_ns = out.events.empty() ? 0U : out.events[out.events.size() - 1U].timestamp_ns;
    }
    out.post_truncated = (out.end == FlightEnd::Shutdown || out.end == FlightEnd::Fatal) &&
                         out.post_retained < impl.policy.post_events;

    // Re-arm at the window's end, unless the capture ended Fatal (terminal). The exchange gives back the sequences the
    // void claims took while the window was frozen (each one a dropped event), so the next kept event is `end`.
    impl.end = FlightEnd::None;
    impl.end_ns = 0U;
    impl.suppressed = 0U;
    impl.suppressed_kinds = 0U;
    const bool fatal = out.end == FlightEnd::Fatal;
    impl.frozen_end.store(fatal ? end : kNoTotal, std::memory_order_relaxed);
    const crd::u64 frozen =
        impl.word.exchange(make_word(end, fatal ? kStateFatalEntered : kStateArmed), std::memory_order_acq_rel);
    out.dropped_while_complete = total_of(frozen) - end;
    return true;
}

// With the collect mutex held: close an open window with `reason` at the current total. A claim that reaches the
// window's last sequence first wins, and the capture ends PostEventsFull.
void close_window(Impl& impl, FlightEnd reason, crd::u64 now_ns) noexcept
{
    crd::u64 word = impl.word.load(std::memory_order_acquire);
    while (state_of(word) == kStateTriggered)
    {
        if (impl.word.compare_exchange_weak(word, make_word(total_of(word), kStateComplete), std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        {
            const crd::u64 post_end = impl.post_end.load(std::memory_order_relaxed);
            const crd::u64 end      = lesser(total_of(word), post_end);
            impl.frozen_end.store(end, std::memory_order_release);
            if (end < post_end)
            {
                impl.end    = reason;
                impl.end_ns = now_ns;
            }
            return;
        }
    }
}

void hand_to_sink(Impl& impl, const FlightCapture& capture) noexcept
{
    const FlightCaptureSink sink = impl.sink.load(std::memory_order_acquire);
    if (sink != nullptr)
    {
        sink(capture, impl.sink_user.load(std::memory_order_acquire));
    }
    impl.delivered.fetch_add(1U, std::memory_order_relaxed);
}
} // namespace

FlightPolicyStatus FlightRecorder::init(const FlightPolicy& policy) noexcept
{
    if (m_impl != nullptr)
    {
        return FlightPolicyStatus::RejectedBusy;
    }
    const FlightPolicyStatus status = validate_flight_policy(policy);
    if (status != FlightPolicyStatus::Committed)
    {
        return status;
    }

    Impl* impl = new (std::nothrow) Impl();
    if (impl == nullptr)
    {
        return FlightPolicyStatus::RejectedOutOfMemory;
    }
    RingStorage storage;
    if (!allocate_storage(storage, policy.capacity))
    {
        delete impl;
        return FlightPolicyStatus::RejectedOutOfMemory;
    }
    impl->id     = g_next_recorder.fetch_add(1U, std::memory_order_relaxed);
    impl->policy = policy;
    impl->keep_from.store(keep_from_of(policy.detail), std::memory_order_release);
    install_storage(*impl, storage);
    impl->replay_length = copy_clipped(impl->replay, kFlightRecipeBytes, kNoRecipe, nullptr);
    add_live(impl);
    m_impl = impl;
    return FlightPolicyStatus::Committed;
}

void FlightRecorder::shutdown() noexcept
{
    if (m_impl == nullptr)
    {
        return;
    }
    (void)finish(FlightEnd::Shutdown);
    remove_live(m_impl);
    free_storage(m_impl->storage);
    delete m_impl;
    m_impl = nullptr;
}

bool FlightRecorder::is_initialized() const noexcept
{
    return m_impl != nullptr;
}

FlightPolicyStatus FlightRecorder::reload_policy(const FlightPolicy& candidate) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return FlightPolicyStatus::RejectedNotInitialized;
    }
    const FlightPolicyStatus status = validate_flight_policy(candidate);
    if (status != FlightPolicyStatus::Committed)
    {
        return status;
    }

    std::lock_guard<std::mutex> collect_guard(impl->collect_mutex);
    std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
    if (state_of(impl->word.load(std::memory_order_acquire)) != kStateArmed)
    {
        return FlightPolicyStatus::RejectedBusy;
    }
    if (impl->policy_generation == 0xFFFFFFFFU)
    {
        return FlightPolicyStatus::RejectedGenerationWrap;
    }

    RingStorage fresh;
    if (!allocate_storage(fresh, candidate.capacity))
    {
        return FlightPolicyStatus::RejectedOutOfMemory;
    }
    // Further claims wait, and every claim in flight finishes, so no claimer touches the rings, `base` or the capacity
    // while they change. The flag leaves the state alone; the claims that meet it claim again after the reload, whose
    // exchange gives back the sequences they took.
    const crd::u64 total = total_of(impl->word.fetch_or(kReloadingBit, std::memory_order_acq_rel));
    impl->reload_base.store(total, std::memory_order_release);
    (void)wait_rings_idle(*impl, nullptr);
    while (impl->shared_inflight.load(std::memory_order_acquire) != 0U)
    {
        std::this_thread::yield();
    }
    RingStorage old = impl->storage;
    install_storage(*impl, fresh);
    free_storage(old);
    impl->base   = total;
    impl->policy = candidate;
    impl->keep_from.store(keep_from_of(candidate.detail), std::memory_order_release);
    ++impl->policy_generation;
    impl->release_epoch.fetch_add(1U, std::memory_order_release); // a thread on the shared ring may now find storage
    impl->reload_base.store(kNoTotal, std::memory_order_relaxed);
    (void)impl->word.exchange(make_word(total, kStateArmed), std::memory_order_acq_rel);
    return FlightPolicyStatus::Committed;
}

FlightPolicy FlightRecorder::policy() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return FlightPolicy{};
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    return impl->policy;
}

crd::u32 FlightRecorder::policy_generation() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return 0U;
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    return impl->policy_generation;
}

void FlightRecorder::set_sink(FlightCaptureSink sink, void* user) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return;
    }
    impl->sink_user.store(user, std::memory_order_release);
    impl->sink.store(sink, std::memory_order_release);
}

void FlightRecorder::set_replay_recipe(cont::StringView recipe) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    impl->replay_length    = copy_clipped(impl->replay, kFlightRecipeBytes, recipe, nullptr);
    impl->replay_available = true;
}

void FlightRecorder::set_replay_unavailable(cont::StringView reason) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    impl->replay_length    = copy_clipped(impl->replay, kFlightRecipeBytes, reason.empty() ? kNoRecipe : reason,
                                          nullptr);
    impl->replay_available = false;
}

// Never inlined: the thread's ring binding is a thread-local read, which a caller that switches job fibers between two
// events must not keep across the switch (the fiber may resume on another thread).
CRD_NOINLINE void FlightRecorder::record(EventCode code, Severity severity, cont::StringView message, crd::u64 value_a,
                                         crd::u64 value_b, RedactionClass redaction) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr) // early-init / late-shutdown: a no-op, never a fault
    {
        return;
    }
    // Outside the active detail's scope: not recorded, not in the sequence, counted, and decided before the clock is
    // read or a sequence claimed.
    if (static_cast<crd::u8>(severity) < impl->keep_from.load(std::memory_order_acquire))
    {
        impl->filtered.add();
        return;
    }
    const crd::u64 now   = diagnostic_now_ns();
    ThreadRing*    ring  = thread_ring(*impl);
    const Claim    claim = begin_claim(*impl, ring);
    if (claim.kept)
    {
        publish(*impl, ring, claim.seq, now, code, severity, message, value_a, value_b, redaction);
    }
    end_claim(*impl, ring, claim);
}

void FlightRecorder::record(const DiagnosticEvent& e) noexcept
{
    record(e.code, e.severity, cont::StringView{e.message.c_str(), e.message.size()}, 0U, 0U, e.redaction);
}

bool FlightRecorder::admit(Severity severity) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    if (static_cast<crd::u8>(severity) < impl->keep_from.load(std::memory_order_acquire))
    {
        impl->filtered.add();
        return false;
    }
    return true;
}

FlightTriggerResult FlightRecorder::trigger(const FlightTriggerInput& input) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return FlightTriggerResult::NotInitialized;
    }
    const crd::u64 now = diagnostic_now_ns();

    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    if (!known_trigger(static_cast<crd::u32>(input.kind)) ||
        (impl->policy.trigger_mask & flight_trigger_bit(input.kind)) == 0U)
    {
        return FlightTriggerResult::Masked;
    }
    // Only claims move the word while Armed and this mutex is held (a reload holds it too), and they only add.
    crd::u64 word = impl->word.load(std::memory_order_acquire);
    if (state_of(word) != kStateArmed)
    {
        ++impl->suppressed;
        impl->suppressed_kinds |= flight_trigger_bit(input.kind);
        return FlightTriggerResult::Suppressed;
    }

    FlightTriggerRecord& t = impl->trigger;
    t.kind                 = input.kind;
    t.code                 = input.code;
    t.timestamp_ns         = now;
    t.value_a              = input.value_a;
    t.value_b              = input.value_b;
    t.line                 = input.source.line;
    t.file_length          = copy_path_tail(t.file, kFlightTriggerFileBytes, input.source.file);
    t.redaction            = input.redaction;
    t.message_length =
        copy_clipped(t.message, kFlightTriggerMessageBytes,
                     input.redaction == RedactionClass::Restricted ? cont::StringView{} : input.message, nullptr);

    std::memcpy(impl->trigger_replay, impl->replay, sizeof(impl->trigger_replay));
    impl->trigger_replay_length    = impl->replay_length;
    impl->trigger_replay_available = impl->replay_available;

    // The exchange fixes the boundary: every claim before it is in the pre window, every claim after it sees
    // Triggered and is in the post window.
    do
    {
        t.sequence = total_of(word);
        impl->post_end.store(t.sequence + impl->policy.post_events, std::memory_order_relaxed);
    } while (!impl->word.compare_exchange_weak(word, make_word(total_of(word), kStateTriggered),
                                               std::memory_order_acq_rel, std::memory_order_acquire));
    return FlightTriggerResult::Started;
}

bool FlightRecorder::poll(crd::u64 now_ns) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }

    FlightCapture capture;
    bool          built = false;
    {
        std::lock_guard<std::mutex> guard(impl->collect_mutex);
        if (state_of(impl->word.load(std::memory_order_acquire)) == kStateTriggered &&
            impl->policy.post_window_ns > 0U && now_ns - impl->trigger.timestamp_ns >= impl->policy.post_window_ns)
        {
            close_window(*impl, FlightEnd::PostWindowElapsed, now_ns);
        }
        if (impl->sink.load(std::memory_order_acquire) == nullptr)
        {
            return state_of(impl->word.load(std::memory_order_acquire)) == kStateComplete; // left for take_capture()
        }
        built = build_capture(*impl, capture, nullptr);
    }
    if (built)
    {
        hand_to_sink(*impl, capture);
    }
    return built;
}

bool FlightRecorder::take_capture(FlightCapture& out) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> guard(impl->collect_mutex);
    const bool built = build_capture(*impl, out, nullptr);
    if (built)
    {
        impl->delivered.fetch_add(1U, std::memory_order_relaxed);
    }
    return built;
}

bool FlightRecorder::finish(FlightEnd reason) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || (reason != FlightEnd::Shutdown && reason != FlightEnd::Fatal))
    {
        return false;
    }
    if (impl->sink.load(std::memory_order_acquire) == nullptr && reason == FlightEnd::Shutdown)
    {
        return false; // nobody to deliver to; take_capture() is the only reader and the recorder is going away
    }

    const crd::u64 now     = diagnostic_now_ns();
    FlightCapture  capture;
    bool           built   = false;
    bool           locked  = false;
    crd::u32       budget  = kFatalWaitAttempts;
    if (reason == FlightEnd::Fatal)
    {
        while (budget > 0U && !locked)
        {
            --budget;
            locked = impl->collect_mutex.try_lock();
            if (!locked)
            {
                std::this_thread::yield();
            }
        }
    }
    else
    {
        impl->collect_mutex.lock();
        locked = true;
    }

    if (locked)
    {
        close_window(*impl, reason, now);
        built = build_capture(*impl, capture, (reason == FlightEnd::Fatal) ? &budget : nullptr);
        impl->collect_mutex.unlock();
    }
    else if (const crd::u32 state = state_of(impl->word.load(std::memory_order_acquire));
             state == kStateTriggered || state == kStateComplete)
    {
        // A collector stays in the way: deliver the trigger alone, said so, rather than nothing.
        std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
        trigger_only_capture(*impl, capture, now);
        built = true;
    }

    if (built)
    {
        hand_to_sink(*impl, capture);
    }
    return built;
}

crd::u64 FlightRecorder::total_recorded() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return 0U;
    }
    // The kept events: the total while armed, up to the post window's end while triggered, the window's end once
    // frozen, a reload's base while it runs (void claims add to the word's total until they are given back).
    for (;;)
    {
        const crd::u64 word = impl->word.load(std::memory_order_acquire);
        crd::u64       kept = kNoTotal;
        if ((word & kReloadingBit) != 0U)
        {
            kept = impl->reload_base.load(std::memory_order_acquire);
        }
        else if (state_of(word) == kStateArmed)
        {
            kept = total_of(word);
        }
        else if (state_of(word) == kStateTriggered)
        {
            kept = lesser(total_of(word), impl->post_end.load(std::memory_order_relaxed));
        }
        else
        {
            kept = impl->frozen_end.load(std::memory_order_acquire);
        }
        if (kept != kNoTotal)
        {
            return kept;
        }
        std::this_thread::yield(); // the thread that froze the window or started the reload is storing it
    }
}

crd::u64 FlightRecorder::total_filtered() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return 0U;
    }
    return impl->filtered.total();
}

bool FlightRecorder::capture_pending() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    const crd::u32 state = state_of(impl->word.load(std::memory_order_acquire));
    return state == kStateTriggered || state == kStateComplete;
}

bool FlightRecorder::fatal_entered() const noexcept
{
    Impl* impl = m_impl;
    return impl != nullptr && state_of(impl->word.load(std::memory_order_acquire)) == kStateFatalEntered;
}

crd::u64 FlightRecorder::captures_delivered() const noexcept
{
    Impl* impl = m_impl;
    return (impl != nullptr) ? impl->delivered.load(std::memory_order_relaxed) : 0U;
}

FlightRingStats FlightRecorder::ring_stats() const noexcept
{
    FlightRingStats stats;
    Impl*           impl = m_impl;
    if (impl == nullptr)
    {
        return stats;
    }
    {
        std::lock_guard<std::mutex> guard(impl->trigger_mutex); // a reload swaps the storage under it
        stats.thread_rings = impl->storage.rings;
        stats.ring_bytes   = impl->storage.bytes;
    }
    for (const ThreadRing& r : impl->rings)
    {
        stats.threads_bound += (r.owner.load(std::memory_order_relaxed) != 0U) ? 1U : 0U;
    }
    stats.shared_events = impl->shared_events.load(std::memory_order_relaxed);
    return stats;
}

#if CRD_ENABLE_ASSERTS
bool FlightRecorder::test_set_policy_generation(crd::u32 generation) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || generation == 0U)
    {
        return false;
    }
    std::lock_guard<std::mutex> collect_guard(impl->collect_mutex);
    std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
    impl->policy_generation = generation;
    return true;
}

bool FlightRecorder::test_with_ring_held(void (*fn)(void* user), void* user) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || fn == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> collect_guard(impl->collect_mutex);
    fn(user);
    return true;
}

bool FlightRecorder::test_with_claim_unpublished(void (*fn)(void* user), void* user) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || fn == nullptr)
    {
        return false;
    }
    ThreadRing* const ring  = thread_ring(*impl);
    const Claim       claim = begin_claim(*impl, ring);
    if (!claim.kept)
    {
        end_claim(*impl, ring, claim);
        return false;
    }
    fn(user);
    publish(*impl, ring, claim.seq, diagnostic_now_ns(), kFlightCodeManual, Severity::Info,
            cont::StringView{"claim held"}, claim.seq, 0U);
    end_claim(*impl, ring, claim);
    return true;
}

void FlightRecorder::test_refuse_ring_storage(crd::u32 count) noexcept
{
    g_refuse_storage.store(count, std::memory_order_relaxed);
}
#endif

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace
{
constexpr crd::u32 kRecordFlagPostTruncated     = 1U << 0U;
constexpr crd::u32 kRecordFlagRingUnavailable   = 1U << 1U;
constexpr crd::u32 kRecordFlagReplay            = 1U << 2U;
constexpr crd::u32 kRecordFlagTriggerRestricted = 1U << 3U; // the trigger is Restricted: it has no message
constexpr crd::u32 kRecordKnownFlags            = 0x0FU;
// An event's flags byte (records before redaction classes held 0 or 1 there, the clip flag).
constexpr crd::u8 kEventFlagTruncated  = 1U << 0U;
constexpr crd::u8 kEventFlagRestricted = 1U << 1U;
constexpr crd::u8 kEventKnownFlags     = 0x03U;

FlightRecordRead refuse(FlightRecordRead& r, const char* why)
{
    r.ok    = false;
    r.error = cont::StringView{why};
    r.capture.events.clear();
    return static_cast<FlightRecordRead&&>(r);
}
} // namespace

cont::Array<crd::u8> encode_flight_record(const FlightCapture& c, crd::memory::IAllocator* alloc)
{
    cont::Array<crd::u8> out(alloc);
    out.reserve(160U + kFlightTriggerFileBytes + kFlightTriggerMessageBytes + kFlightRecipeBytes +
                c.events.size() * (40U + kFlightMessageBytes));

    put_u32(out, kFlightRecordMagic);
    put_u32(out, kFlightRecordVersion);

    crd::u32 flags = 0U;
    flags |= c.post_truncated ? kRecordFlagPostTruncated : 0U;
    flags |= c.ring_unavailable ? kRecordFlagRingUnavailable : 0U;
    flags |= c.replay_available ? kRecordFlagReplay : 0U;
    flags |= c.trigger.redaction == RedactionClass::Restricted ? kRecordFlagTriggerRestricted : 0U;
    put_u8(out, static_cast<crd::u8>(c.trigger.kind));
    put_u8(out, static_cast<crd::u8>(c.end));
    put_u8(out, static_cast<crd::u8>(flags));
    put_u8(out, static_cast<crd::u8>(c.policy.detail)); // the pad of records before detail levels: 0 is Full

    put_u32(out, c.trigger.code);
    put_u64(out, c.trigger.sequence);
    put_u64(out, c.trigger.timestamp_ns);
    put_u64(out, c.trigger.value_a);
    put_u64(out, c.trigger.value_b);
    put_u32(out, c.trigger.line);
    put_u16(out, c.trigger.file_length);
    put_u16(out, c.trigger.message_length);
    put_bytes(out, c.trigger.file, c.trigger.file_length);
    put_bytes(out, c.trigger.message, c.trigger.message_length);
    put_u64(out, c.end_ns);

    put_u32(out, c.policy.schema_version);
    put_u32(out, c.policy.capacity);
    put_u32(out, c.policy.pre_events);
    put_u32(out, c.policy.post_events);
    put_u64(out, c.policy.post_window_ns);
    put_u32(out, c.policy.trigger_mask);
    put_u64(out, c.policy.hitch_threshold_ns);
    put_u32(out, c.policy_generation);

    put_u64(out, c.recorded_before_trigger);
    put_u32(out, c.pre_retained);
    put_u32(out, c.post_retained);
    put_u64(out, c.dropped_while_complete);
    put_u64(out, c.suppressed_triggers);
    put_u32(out, c.suppressed_kinds);

    const crd::usize replay_n = (c.replay.size() < kFlightRecipeBytes) ? c.replay.size() : kFlightRecipeBytes - 1U;
    put_u16(out, static_cast<crd::u16>(replay_n));
    put_bytes(out, c.replay.c_str(), replay_n);

    put_u32(out, static_cast<crd::u32>(c.events.size()));
    for (crd::usize i = 0U; i < c.events.size(); ++i)
    {
        const FlightEvent& e = c.events[i];
        put_u64(out, e.sequence);
        put_u64(out, e.timestamp_ns);
        put_u64(out, e.value_a);
        put_u64(out, e.value_b);
        put_u32(out, e.code);
        put_u8(out, static_cast<crd::u8>(e.severity));
        put_u8(out, static_cast<crd::u8>((e.message_truncated ? kEventFlagTruncated : 0U) |
                                         (e.redaction == RedactionClass::Restricted ? kEventFlagRestricted : 0U)));
        put_u16(out, e.message_length);
        put_bytes(out, e.message, e.message_length);
    }
    return out;
}

FlightRecordRead decode_flight_record(cont::ConstSpan<crd::u8> payload)
{
    FlightRecordRead r;
    Cursor           in{payload};

    if (in.u32() != kFlightRecordMagic || !in.ok)
    {
        return refuse(r, "not a flight record (magic)");
    }
    if (in.u32() != kFlightRecordVersion)
    {
        return refuse(r, "unsupported flight record version");
    }

    FlightCapture& c   = r.capture;
    const crd::u8  kind  = in.u8();
    const crd::u8  end   = in.u8();
    const crd::u8  flags  = in.u8();
    const crd::u8  detail = in.u8();
    if (!in.ok || !known_trigger(kind) || end > static_cast<crd::u8>(FlightEnd::Fatal) || end == 0U ||
        (flags & ~kRecordKnownFlags) != 0U)
    {
        return refuse(r, "bad trigger kind, end reason or flags");
    }
    if (detail > kFlightMaxDetail)
    {
        return refuse(r, "unknown detail level");
    }
    c.trigger.kind     = static_cast<FlightTrigger>(kind);
    c.end              = static_cast<FlightEnd>(end);
    c.post_truncated   = (flags & kRecordFlagPostTruncated) != 0U;
    c.ring_unavailable = (flags & kRecordFlagRingUnavailable) != 0U;
    c.replay_available = (flags & kRecordFlagReplay) != 0U;
    c.trigger.redaction =
        (flags & kRecordFlagTriggerRestricted) != 0U ? RedactionClass::Restricted : RedactionClass::Public;

    c.trigger.code           = in.u32();
    c.trigger.sequence       = in.u64();
    c.trigger.timestamp_ns   = in.u64();
    c.trigger.value_a        = in.u64();
    c.trigger.value_b        = in.u64();
    c.trigger.line           = in.u32();
    c.trigger.file_length    = in.u16();
    c.trigger.message_length = in.u16();
    if (!in.text(c.trigger.file, kFlightTriggerFileBytes, c.trigger.file_length) ||
        !in.text(c.trigger.message, kFlightTriggerMessageBytes, c.trigger.message_length))
    {
        return refuse(r, "trigger text over its bound or past the payload");
    }
    if (c.trigger.redaction == RedactionClass::Restricted && c.trigger.message_length != 0U)
    {
        return refuse(r, "a restricted trigger with a message");
    }
    c.end_ns = in.u64();

    c.policy.schema_version     = in.u32();
    c.policy.capacity           = in.u32();
    c.policy.pre_events         = in.u32();
    c.policy.post_events        = in.u32();
    c.policy.post_window_ns     = in.u64();
    c.policy.trigger_mask       = in.u32();
    c.policy.hitch_threshold_ns = in.u64();
    c.policy.detail             = static_cast<FlightDetail>(detail);
    c.policy_generation         = in.u32();
    if (!in.ok || validate_flight_policy(c.policy) != FlightPolicyStatus::Committed)
    {
        return refuse(r, "recorded policy out of bounds");
    }

    c.recorded_before_trigger = in.u64();
    c.pre_retained            = in.u32();
    c.post_retained           = in.u32();
    c.dropped_while_complete  = in.u64();
    c.suppressed_triggers     = in.u64();
    c.suppressed_kinds        = in.u32();
    if (!in.ok || c.pre_retained > c.policy.pre_events || c.post_retained > c.policy.post_events ||
        c.pre_retained > c.recorded_before_trigger || c.recorded_before_trigger != c.trigger.sequence ||
        (c.suppressed_kinds & ~kFlightAllTriggers) != 0U)
    {
        return refuse(r, "inconsistent loss metadata");
    }
    // What a recorder writes and nothing else (DIAG.10a: a record that contradicts itself is not evidence). trigger()
    // starts or suppresses only a kind the policy arms, and counts every suppression with its kind; build_capture() and
    // finish() derive the end and the truncation from the post window: it ends post-events-full exactly when
    // post_events events landed, it can elapse only under a time bound, and a capture without the ring is a fatal one
    // with no events and no drop count.
    if ((c.policy.trigger_mask & flight_trigger_bit(c.trigger.kind)) == 0U ||
        (c.suppressed_kinds & ~c.policy.trigger_mask) != 0U ||
        (c.suppressed_triggers == 0U) != (c.suppressed_kinds == 0U))
    {
        return refuse(r, "the trigger or a suppressed kind is not armed by the recorded policy, or the suppression "
                         "count disagrees with its kinds");
    }
    const bool ended_early = c.end == FlightEnd::Shutdown || c.end == FlightEnd::Fatal;
    if ((c.end == FlightEnd::PostEventsFull) != (c.post_retained == c.policy.post_events) ||
        c.post_truncated != (ended_early && c.post_retained < c.policy.post_events) ||
        (c.end == FlightEnd::PostWindowElapsed && c.policy.post_window_ns == 0U))
    {
        return refuse(r, "the end reason disagrees with the post window");
    }
    if (c.ring_unavailable &&
        (c.end != FlightEnd::Fatal || c.pre_retained != 0U || c.post_retained != 0U || c.dropped_while_complete != 0U))
    {
        return refuse(r, "a capture without the ring must be fatal and hold no events");
    }

    char           replay[kFlightRecipeBytes] = {};
    const crd::u16 replay_n                   = in.u16();
    if (!in.text(replay, kFlightRecipeBytes, replay_n))
    {
        return refuse(r, "replay text over its bound or past the payload");
    }
    if (!c.replay_available && replay_n == 0U)
    {
        return refuse(r, "replay unavailable without its reason");
    }
    c.replay.clear();
    c.replay.append(cont::StringView{replay, replay_n});

    const crd::u32 count = in.u32();
    if (!in.ok || count != c.pre_retained + c.post_retained || count > kFlightMaxEvents)
    {
        return refuse(r, "event count disagrees with the retained counts");
    }
    // Each event takes at least 40 bytes, so a count the payload cannot hold is refused before reserving for it.
    if ((payload.size() - in.at) / 40U < count)
    {
        return refuse(r, "event count past the payload");
    }
    c.events.reserve(count);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        FlightEvent e;
        e.sequence     = in.u64();
        e.timestamp_ns = in.u64();
        e.value_a      = in.u64();
        e.value_b      = in.u64();
        e.code         = in.u32();
        const crd::u8  severity    = in.u8();
        const crd::u8  event_flags = in.u8();
        const crd::u16 length      = in.u16();
        if (!in.ok || severity > static_cast<crd::u8>(Severity::FatalInvariant) ||
            (event_flags & ~kEventKnownFlags) != 0U || !in.text(e.message, kFlightMessageBytes, length))
        {
            return refuse(r, "malformed event");
        }
        e.severity          = static_cast<Severity>(severity);
        e.message_length    = static_cast<crd::u8>(length); // in.text() bounded it by kFlightMessageBytes
        e.message_truncated = (event_flags & kEventFlagTruncated) != 0U;
        e.redaction = (event_flags & kEventFlagRestricted) != 0U ? RedactionClass::Restricted : RedactionClass::Public;
        // The capture copies one contiguous run of the ring, from pre_retained before the trigger's sequence on.
        if (e.sequence != c.trigger.sequence - c.pre_retained + i)
        {
            return refuse(r, "event sequence not contiguous across the trigger");
        }
        // A clipped message fills its buffer, a restricted event has none, and a Basic policy records Warning and
        // above only.
        if ((e.message_truncated && e.message_length != kFlightMessageBytes - 1U) ||
            (e.redaction == RedactionClass::Restricted && (e.message_length != 0U || e.message_truncated)) ||
            (c.policy.detail == FlightDetail::Basic && e.severity < Severity::Warning))
        {
            return refuse(r, "an event the recorder could not have written (clip flag, redaction or detail level)");
        }
        c.events.push_back(e);
    }
    if (in.at != payload.size())
    {
        return refuse(r, "trailing bytes after the last event");
    }
    r.ok = true;
    return r;
}

cont::Array<crd::u8> write_flight_bundle(const FlightCapture& capture, cont::ConstSpan<ModuleIdentity> modules)
{
    BundleManifest manifest;
    manifest.schema_version = kDiagnosticSchemaVersion;
    if (modules.empty())
    {
        manifest.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::SymbolIndex));
    }
    const cont::Array<crd::u8> manifest_bytes = serialize_manifest(manifest);
    const cont::Array<crd::u8> record_bytes   = encode_flight_record(capture);
    const cont::Array<crd::u8> build_bytes    = encode_build_identity(current_build_identity());
    const cont::Array<crd::u8> hardware_bytes = encode_hardware_identity(host_hardware_identity());

    BundleWriter writer;
    (void)writer.add_section(BundleSectionTag::Manifest,
                             cont::ConstSpan<crd::u8>{manifest_bytes.data(), manifest_bytes.size()});
    (void)writer.add_section(BundleSectionTag::FlightRecord,
                             cont::ConstSpan<crd::u8>{record_bytes.data(), record_bytes.size()});
    (void)writer.add_section(BundleSectionTag::BuildIdentity,
                             cont::ConstSpan<crd::u8>{build_bytes.data(), build_bytes.size()});
    (void)writer.add_section(BundleSectionTag::HardwareIdentity,
                             cont::ConstSpan<crd::u8>{hardware_bytes.data(), hardware_bytes.size()});
    if (modules.empty())
    {
        (void)writer.add_absent(BundleSectionTag::SymbolIndex);
    }
    else
    {
        const cont::Array<crd::u8> symbol_bytes = serialize_symbol_index(modules);
        (void)writer.add_section(BundleSectionTag::SymbolIndex,
                                 cont::ConstSpan<crd::u8>{symbol_bytes.data(), symbol_bytes.size()});
    }
    return writer.finish();
}

void flight_capture_summary_json(const FlightCapture& c, cont::String& out)
{
    const auto key = [&out](const char* name, bool comma)
    {
        if (comma)
        {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(name);
        out.append("\":");
    };
    const auto str = [&out](cont::StringView v)
    {
        out.push_back('"');
        append_json_escaped(out, v);
        out.push_back('"');
    };

    out.push_back('{');
    key("trigger", false);
    str(flight_trigger_name(c.trigger.kind));
    key("code", true);
    append_u64(out, c.trigger.code);
    key("sequence", true);
    append_u64(out, c.trigger.sequence);
    key("message", true);
    str(c.trigger.message_view());
    key("redaction", true);
    str(redaction_class_name(c.trigger.redaction));
    key("file", true);
    str(c.trigger.file_view());
    key("line", true);
    append_u64(out, c.trigger.line);
    key("value_a", true);
    append_u64(out, c.trigger.value_a);
    key("value_b", true);
    append_u64(out, c.trigger.value_b);
    key("end", true);
    str(flight_end_name(c.end));
    key("pre_retained", true);
    append_u64(out, c.pre_retained);
    key("pre_dropped", true);
    append_u64(out, c.recorded_before_trigger - c.pre_retained);
    key("post_retained", true);
    append_u64(out, c.post_retained);
    key("post_truncated", true);
    out.append(c.post_truncated ? "true" : "false");
    key("ring_unavailable", true);
    out.append(c.ring_unavailable ? "true" : "false");
    key("dropped_while_complete", true);
    append_u64(out, c.dropped_while_complete);
    key("suppressed_triggers", true);
    append_u64(out, c.suppressed_triggers);
    key("policy_generation", true);
    append_u64(out, c.policy_generation);
    key("detail", true);
    str(flight_detail_name(c.policy.detail));
    key("replay_available", true);
    out.append(c.replay_available ? "true" : "false");
    key("replay", true);
    str(cont::StringView{c.replay.c_str(), c.replay.size()});
    out.push_back('}');
}

} // namespace crd::perf
