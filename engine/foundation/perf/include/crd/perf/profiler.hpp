#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- Profiler substrate public surface (Detour D-003).
//
// One singleton Profiler per process. Initialised at startup, shut down at
// teardown. Hot paths:
//
//   - push_region(NameId, Category, color)  -- enter scope
//   - pop_region(begin_ts)                  -- exit scope, write Sample
//   - frame_mark()                          -- frame boundary; swap snapshot
//
// Thread registration: each thread that profiles must call register_thread()
// once before its first scope. The init() routine registers the main thread
// automatically.
//
// Name interning: intern_name(const char*) returns a NameId; the macro
// CRD_PERF_SCOPE caches the id in a TU-local static so the lookup happens
// once per call site at first hit. Internal table is mutex-protected on the
// cold path; the hot path is one indexed read.
//
// When CRD_PERF_ENABLED=0, every method is either inline-empty or returns
// a sentinel; the substrate state is reduced to a single bool. Zero
// overhead is verified by the v0a objdump-equality test.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/sample.hpp>
#include <crd/time/clocks.hpp>

namespace crd::perf
{

// Forward decls (kept opaque in the header).
namespace detail
{
struct ProfilerState;

// DIAG.6a(e): pin the profiler state across a multi-call read (e.g. a whole capture) so a concurrent shutdown() cannot
// free it mid-read. state_read_acquire() increments a global in-flight count and returns true ONLY if the profiler is
// active (a false return holds no pin -- do not release it). shutdown() retires the state pointer, then drains this
// count to zero before delete, so a reader that acquired first is waited for and one that arrives after sees inactive.
// Prefer the RAII StateReadGuard. Do NOT call shutdown() while holding one (self-deadlock). Defined in both build
// configurations (a no-op pin when CRD_PERF_ENABLED=0).
[[nodiscard]] bool state_read_acquire() noexcept;
void               state_read_release() noexcept;

struct StateReadGuard
{
    bool held;
    StateReadGuard() noexcept : held(state_read_acquire()) {}
    ~StateReadGuard() noexcept
    {
        if (held)
        {
            state_read_release();
        }
    }
    StateReadGuard(const StateReadGuard&)            = delete;
    StateReadGuard& operator=(const StateReadGuard&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return held; }
};
} // namespace detail

// ---- Init / shutdown ----------------------------------------------------

struct InitConfig
{
    // Override per-thread ring slot count at startup. 0 = use kPerThreadRingSlots.
    crd::u32 per_thread_ring_slots = 0U;

    // Override interned-name capacity. 0 = use kMaxRegionNames.
    crd::u32 max_region_names = 0U;

    // Override frame-history slot count. 0 = use kFrameHistorySlots.
    crd::u32 frame_history_slots = 0U;
};

// Initialise the profiler. Safe to call once per process. If
// CRD_PERF_ENABLED=0 this is a no-op and the singleton is never built.
void init(const InitConfig& cfg = {});

// Shutdown -- frees all internal storage. Safe to call multiple times.
void shutdown();

// True iff init() has been called and shutdown() has not.
[[nodiscard]] bool is_active() noexcept;

// ---- Name interning -----------------------------------------------------

// Intern a region name; returns a stable NameId. DIAG.6a(d): the profiler COPIES the bytes into its own arena on first
// insert, so `name` need NOT outlive the call -- a dynamically-built name, or one whose bytes live in a module later
// unloaded, is safe (the copy never dangles). Content-keyed (FNV-1a + strcmp): equal bytes return the same id; a reused
// buffer holding different bytes returns a different id. If the name table or the arena is exhausted, or the name
// exceeds kMaxNameBytes, this returns kInvalidNameId and bumps name_bytes_dropped_count() -- never a silent borrow.
// The macro CRD_PERF_SCOPE caches the result in a TU-local static, so the per-call-site cost is one branch-predicted
// load after the first hit (the byte copy happens once, on the cold insert).
[[nodiscard]] NameId intern_name(const char* name) noexcept;

// Resolve a NameId to its interned string (for UI / capture I/O). The pointer is into the profiler's owned name arena
// and stays valid until shutdown(); safe cross-thread (lock-free acquire-load). Returns "" for invalid ids.
[[nodiscard]] const char* resolve_name(NameId id) noexcept;

// Capacity of the interned-name table (i.e. valid NameId index range
// is [0, intern_name_capacity())). Sparse: not every slot is filled.
// Used by the CPROF capture writer to walk every potentially-filled
// slot and pack the name blob.
[[nodiscard]] crd::u32 intern_name_capacity() noexcept;

// Number of currently-interned names (filled slots). <= capacity.
[[nodiscard]] crd::u32 intern_name_count() noexcept;

// DIAG.6a(d): count of names the profiler could not own (name table full, name arena full, or a name longer than
// kMaxNameBytes) across region/thread/counter/allocator registration. Non-zero means at least one name is unavailable
// (kInvalidNameId, or the stable "(name storage exhausted)" literal for the three registries) rather than borrowed.
[[nodiscard]] crd::u64 name_bytes_dropped_count() noexcept;

// ---- Thread registration ------------------------------------------------

// Register the calling thread under `name`. Returns the OS-thread index
// the profiler assigned (also accessible via current_thread_index()).
// Idempotent: re-registering returns the existing index.
//
// Sample::begin_thread / end_thread store this index as a u8.
crd::u8 register_thread(const char* name) noexcept;

// DIAG.6b(c): register an EXTERNAL track -- a dedicated ring slot that is NOT the calling thread's own track and does
// NOT touch thread-local state. Every call allocates a fresh slot (no idempotent refresh -- the caller owns the mapping
// from its logical identity to the returned index). Returns kInvalidThread (0xFF) if the profiler is down or the
// kMaxThreads table is full; unlike register_thread it degrades quietly rather than asserting, so a producer that opens
// many tracks (e.g. per GPU device/queue, DIAG.6b(c)) can fall back instead of taking the process down. Samples reach
// the slot only via detail::write_external_sample(idx, ...); the standard push/pop path writes the caller's own ring.
crd::u8 register_external_track(const char* name) noexcept;

// Index of the calling thread (or 0xFF if not registered).
[[nodiscard]] crd::u8 current_thread_index() noexcept;

// Set the fiber id of the current logical task on this thread. The next
// push_region() captures it into Sample::fiber_id. 0 = "no fiber / OS
// thread context". v0c calls this from the JobObserver around fiber
// suspend / resume.
void set_current_fiber_id(crd::u32 fiber_id) noexcept;

[[nodiscard]] crd::u32 current_fiber_id() noexcept;

// ---- Scope push / pop (hot path) ----------------------------------------
//
// push_region returns a BeginToken capturing (begin_ns, begin_thread,
// begin_fiber). pop_region consumes it and writes the completed Sample
// into the calling thread's ring -- with both begin and end thread ids,
// so fiber migration is recorded faithfully.
//
// Inline-empty when CRD_PERF_ENABLED=0.

struct BeginToken
{
    crd::i64 begin_ns      = 0;
    crd::u32 begin_fiber   = 0U;
    crd::u8  begin_thread  = 0xFFU;
    crd::u8  depth         = 0U;
    crd::u16 pad           = 0U;
};
static_assert(sizeof(BeginToken) == 16, "BeginToken is 16 bytes; lives on the stack");

#if CRD_PERF_ENABLED
[[nodiscard]] BeginToken push_region(NameId id, Category cat = Category::User,
                                     crd::u32 color_rgba = 0U) noexcept;
void pop_region(NameId id, BeginToken begin, Category cat = Category::User,
                crd::u32 color_rgba = 0U) noexcept;
#else
[[nodiscard]] inline BeginToken push_region(NameId, Category = Category::User,
                                            crd::u32 = 0U) noexcept
{
    return BeginToken{};
}
inline void pop_region(NameId, BeginToken, Category = Category::User,
                       crd::u32 = 0U) noexcept
{
}
#endif

// ---- Frame boundary -----------------------------------------------------

// Mark the end of one rendering / simulation frame. Swaps the front /
// back frame-snapshot buffers, advances the rolling history ring, and
// signals to any attached file capture that a frame boundary has passed.
//
// Cheap; safe to call once per Application::tick().
#if CRD_PERF_ENABLED
void frame_mark() noexcept;
#else
inline void frame_mark() noexcept {}
#endif

// Number of frames marked since init().
[[nodiscard]] crd::u64 frame_count() noexcept;

// Index of the currently-recording frame (= frame_count() while a frame
// is in flight). Useful for tagging GPU spans.
[[nodiscard]] crd::u64 current_frame_index() noexcept;

// ---- Introspection (for v0g UI + v0f file capture) ----------------------

// Zero-copy snapshot of a single thread's ring. Cross-thread safety is per FIELD, not blanket:
//   - `dropped` (atomic stat) and `name` (a literal/interned pointer published before the ring's `active` flag) are
//     safe to read from any thread; `size` is a safe cross-thread ESTIMATE of head - tail.
//   - Indexing `data[0..size)` is SAME-THREAD ONLY and only correct while the ring has not wrapped (tail == 0): `data`
//     is the ring's BASE, so after a clear_samples()/wrap the live samples live at [tail & mask, head & mask) and are
//     NOT contiguous from `data[0]` -- indexing then reads the wrong slots (DIAG.6a(c2)).
// Any cross-thread reader of the SAMPLES, or any reader after a possible wrap, MUST use copy_thread_samples, which
// returns them oldest-first regardless of wrap and refuses to race a concurrent consumer (DIAG.6a(c3)).
struct ThreadSamplesView
{
    const Sample* data;          // ring BASE -- same-thread + unwrapped indexing only (use copy_thread_samples else)
    crd::u32      size;          // head - tail (cross-thread estimate; see the wrap caveat above)
    crd::u32      dropped;       // total samples dropped to overflow since init() (atomic stat; cross-thread safe)
    const char*   name;          // thread name (may be nullptr; published before `active`, cross-thread safe)
};

[[nodiscard]] ThreadSamplesView thread_samples(crd::u8 thread_index) noexcept;

// Cross-thread-safe, wrap-correct sample read: copies up to `max_samples` of thread `thread_index`'s live samples
// (oldest first) into `out`, returning the count copied. Handles the ring wrap the zero-copy view cannot.
//
// (c3) ENFORCED SINGLE CONSUMER. Exactly one consumer -- a copier OR clear_samples -- may be in a given ring at a
// time. If another consumer already holds the ring this refuses rather than races: it returns 0 and, when
// `out_contended` is non-null, sets *out_contended = true. (A return of 0 with *out_contended == false means the ring
// was genuinely empty.) Callers that must not silently drop samples -- the capture writer -- pass `out_contended` and
// retry; a live UI view may pass nullptr and tolerate an occasional skipped frame. sample_copy_contended_count gives
// the exact number of refusals so contention is never invisible.
[[nodiscard]] crd::u32 copy_thread_samples(crd::u8 thread_index, Sample* out, crd::u32 max_samples,
                                           bool* out_contended = nullptr) noexcept;

// (c3) Count of copy_thread_samples attempts refused because another consumer held thread `thread_index`'s ring.
// Exactly equals the number of contended (out_contended == true) returns; monotonic within an init/shutdown lifetime.
[[nodiscard]] crd::u64 sample_copy_contended_count(crd::u8 thread_index) noexcept;

// (c3) Copy priority for a save. A save whose copy found thread `thread_index`'s ring busy calls begin while it retries
// and end when it is done. In between, every other copier steps aside: it refuses exactly as if the ring were busy, so
// consumers that copy back to back cannot starve a save for its whole retry budget (a hosted ThreadSanitizer run did).
// The save waits at most for the copy already in flight. No-ops when CRD_PERF_ENABLED=0.
void sample_copy_priority_begin(crd::u8 thread_index) noexcept;
void sample_copy_priority_end(crd::u8 thread_index) noexcept;

// (c3) Slot capacity of each per-thread sample ring -- the maximum a copy_thread_samples can return, and the size a
// cross-thread reader must size its buffer to in order to hold a full ring. Reflects InitConfig::per_thread_ring_slots
// (or kPerThreadRingSlots by default); 0 when the profiler is inactive.
[[nodiscard]] crd::u32 per_thread_ring_capacity() noexcept;

// DIAG.6b(b): opt thread `thread_index` into per-slot GPU/queue correlation -- allocates a CorrelationRecord array
// slot-parallel to its sample ring. Cold path, idempotent; call once at (gpu-)thread registration. Only
// write_external_sample writes the records (the gpu track today); CPU producers are DIAG.6b(c)/(e).
void enable_thread_correlation(crd::u8 thread_index) noexcept;

// True if `thread_index` has correlation enabled (used by the capture writer to size/skip the correlation section).
[[nodiscard]] bool thread_has_correlation(crd::u8 thread_index) noexcept;

// DIAG.6b(b): like copy_thread_samples, but also copies each sample's slot-parallel CorrelationRecord into `corr_out`
// under the SAME single-consumer hold -- out[i] and corr_out[i] describe the same sample (wrap-safe, no key). `corr_out`
// must hold at least `max_samples` records. A thread without correlation yields cleared records (all-zero = none).
// Same contention contract as copy_thread_samples (returns 0 + sets *out_contended when another consumer holds the ring).
[[nodiscard]] crd::u32 copy_thread_samples_with_correlation(crd::u8 thread_index, Sample* out,
                                                            CorrelationRecord* corr_out, crd::u32 max_samples,
                                                            bool* out_contended = nullptr) noexcept;

[[nodiscard]] crd::u32 thread_count() noexcept;

// Clear all ring buffers (does NOT reset frame_count). Used between
// capture start/stop transitions.
void clear_samples() noexcept;

} // namespace crd::perf
