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
}

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

// Intern a static-string region name. `static_name` must outlive the
// profiler (string literals are the only well-formed input). Returns a
// stable NameId. Subsequent calls with the same pointer return the same
// id (pointer-keyed; no hashing needed in the common case).
//
// The macro CRD_PERF_SCOPE caches the result in a TU-local static so the
// per-call-site cost is one branch-predicted load after the first hit.
[[nodiscard]] NameId intern_name(const char* static_name) noexcept;

// Resolve a NameId back to the originating string (for UI / capture I/O).
// Returns "" for invalid ids.
[[nodiscard]] const char* resolve_name(NameId id) noexcept;

// Capacity of the interned-name table (i.e. valid NameId index range
// is [0, intern_name_capacity())). Sparse: not every slot is filled.
// Used by the CPROF capture writer to walk every potentially-filled
// slot and pack the name blob.
[[nodiscard]] crd::u32 intern_name_capacity() noexcept;

// Number of currently-interned names (filled slots). <= capacity.
[[nodiscard]] crd::u32 intern_name_count() noexcept;

// ---- Thread registration ------------------------------------------------

// Register the calling thread under `name`. Returns the OS-thread index
// the profiler assigned (also accessible via current_thread_index()).
// Idempotent: re-registering returns the existing index.
//
// Sample::begin_thread / end_thread store this index as a u8.
crd::u8 register_thread(const char* name) noexcept;

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
    crd::u16 _pad          = 0U;
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

// Snapshot of a single thread's ring at this instant. Returned pointer
// is valid until the next push_region() on that thread. Callers reading
// from a non-recording thread are safe.
struct ThreadSamplesView
{
    const Sample* data;
    crd::u32      size;          // valid samples (head - tail wrapping handled)
    crd::u32      dropped;       // total samples dropped to overflow since init()
    const char*   name;          // thread name (may be nullptr)
};

[[nodiscard]] ThreadSamplesView thread_samples(crd::u8 thread_index) noexcept;

[[nodiscard]] crd::u32 thread_count() noexcept;

// Clear all ring buffers (does NOT reset frame_count). Used between
// capture start/stop transitions.
void clear_samples() noexcept;

} // namespace crd::perf
