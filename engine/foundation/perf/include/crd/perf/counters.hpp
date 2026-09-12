#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- typed counters substrate (Detour D-003 v0b).
//
// A "counter" is a named, typed value the user updates over the course of
// a frame. At every frame_mark() the profiler snapshots every counter
// into the rolling FrameRecord history (240 frames by default). The UI
// renders counters as line plots over time.
//
// Three types: i64, f64, Duration (stored as i64 ns).
// Two kinds:   Set (overwrite-last-wins)  --  Add (accumulate-within-frame).
//
// Set kind:  `CRD_PERF_COUNTER_SET_I64("draws.this_frame", n);`
//   Each call replaces the current frame value. The snapshot at
//   frame_mark() records the last-set value. Useful for "draw count this
//   frame", "active particle count", "queue depth".
//
// Add kind:  `CRD_PERF_COUNTER_ADD_I64("samples.written", n);`
//   Each call atomically increments the current value. The snapshot at
//   frame_mark() records the accumulated value and resets the counter to
//   zero. Useful for "bytes uploaded this frame", "cache misses".
//
// Hot path per macro call:
//   - first hit:  one register_counter() call (mutex on cold path)
//   - subsequent: one indirect-branch-predictable load of the cached id
//   - update:     one atomic-relaxed store (Set) or fetch_add (Add)
//
// All writes are thread-safe; counters are routinely bumped from job
// fibers running on worker threads.
//
// When CRD_PERF_ENABLED == 0 every macro collapses to ((void)0); the
// counter table is never allocated and the registration calls never
// run. Verified by the v0a zero-overhead-gate test pattern.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/time/duration.hpp>

namespace crd::perf
{

// ---- Counter kind / type --------------------------------------------------

enum class CounterKind : crd::u8
{
    Set = 0, // overwrite-last-wins; survives across frame_mark
    Add = 1, // accumulate-within-frame; reset to zero by frame_mark
};

enum class CounterType : crd::u8
{
    I64        = 0,
    F64        = 1,
    DurationNs = 2, // stored as i64 ns inside u64 bits; decoded back to crd::time::Duration on read
};

// Counter handle. Returned by register_counter_*; cached by the macros
// in TU-local statics so the lookup happens once per call site.
struct CounterId
{
    crd::u32 value = 0xFFFF'FFFFU;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

inline constexpr CounterId kInvalidCounterId{0xFFFF'FFFFU};

// ---- Cold-path registration ----------------------------------------------
//
// `static_name` MUST be a string literal (or stable static-storage const
// char*). Returned id is stable for the lifetime of the profiler.
// Re-registering the same name (same content) returns the same id.

[[nodiscard]] CounterId register_counter_i64(const char* static_name, CounterKind kind) noexcept;
[[nodiscard]] CounterId register_counter_f64(const char* static_name, CounterKind kind) noexcept;
[[nodiscard]] CounterId register_counter_duration(const char* static_name, CounterKind kind) noexcept;

// ---- Hot-path writes ------------------------------------------------------
//
// All writes are atomic-relaxed; safe from any thread. Reading concurrent
// writes during the frame_mark snapshot is benign -- the snapshot is
// "approximately this frame", not strictly happens-before.

#if CRD_PERF_ENABLED

void counter_set_i64(CounterId id, crd::i64 value) noexcept;
void counter_set_f64(CounterId id, crd::f64 value) noexcept;
void counter_set_duration(CounterId id, crd::time::Duration value) noexcept;

void counter_add_i64(CounterId id, crd::i64 delta) noexcept;
void counter_add_f64(CounterId id, crd::f64 delta) noexcept;
void counter_add_duration(CounterId id, crd::time::Duration delta) noexcept;

#else

inline void counter_set_i64(CounterId, crd::i64) noexcept {}
inline void counter_set_f64(CounterId, crd::f64) noexcept {}
inline void counter_set_duration(CounterId, crd::time::Duration) noexcept {}
inline void counter_add_i64(CounterId, crd::i64) noexcept {}
inline void counter_add_f64(CounterId, crd::f64) noexcept {}
inline void counter_add_duration(CounterId, crd::time::Duration) noexcept {}

#endif

// ---- Introspection (for v0g UI + v0f capture) ----------------------------

struct CounterInfo
{
    const char* name;
    CounterKind kind;
    CounterType type;
};

// Number of registered counters (monotonic; counters never unregister).
[[nodiscard]] crd::u32 counter_count() noexcept;

// Read the metadata for one counter. id.value must be < counter_count().
[[nodiscard]] CounterInfo counter_info(CounterId id) noexcept;

// Read the current live value of a counter (relaxed atomic load).
// For Add-kind counters this is the accumulator since the last frame_mark.
[[nodiscard]] crd::i64 counter_current_i64(CounterId id) noexcept;
[[nodiscard]] crd::f64 counter_current_f64(CounterId id) noexcept;
[[nodiscard]] crd::time::Duration counter_current_duration(CounterId id) noexcept;

// Frame-history accessors. `frames_back == 0` returns the most-recently-
// captured FrameRecord; `frames_back == 1` is the previous frame; up to
// kFrameHistorySlots - 1. nullptr if not enough frames have been captured.
[[nodiscard]] const FrameRecord* frame_record(crd::u32 frames_back) noexcept;

// Number of FrameRecords currently retained. Saturates at kFrameHistorySlots.
[[nodiscard]] crd::u32 frame_record_count() noexcept;

} // namespace crd::perf

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define CRD_PERF_COUNTER_DETAIL_CAT_INNER(a, b) a##b
#define CRD_PERF_COUNTER_DETAIL_CAT(a, b) CRD_PERF_COUNTER_DETAIL_CAT_INNER(a, b)

#if CRD_PERF_ENABLED

// Set kind ------------------------------------------------------------------

#define CRD_PERF_COUNTER_SET_I64(name_literal, value)                                                  \
    do                                                                                                 \
    {                                                                                                  \
        static const ::crd::perf::CounterId CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__) =     \
            ::crd::perf::register_counter_i64(name_literal, ::crd::perf::CounterKind::Set);            \
        ::crd::perf::counter_set_i64(CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__), (value));   \
    } while (false)

#define CRD_PERF_COUNTER_SET_F64(name_literal, value)                                                  \
    do                                                                                                 \
    {                                                                                                  \
        static const ::crd::perf::CounterId CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__) =     \
            ::crd::perf::register_counter_f64(name_literal, ::crd::perf::CounterKind::Set);            \
        ::crd::perf::counter_set_f64(CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__), (value));   \
    } while (false)

#define CRD_PERF_COUNTER_SET_DURATION(name_literal, value)                                             \
    do                                                                                                 \
    {                                                                                                  \
        static const ::crd::perf::CounterId CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__) =     \
            ::crd::perf::register_counter_duration(name_literal, ::crd::perf::CounterKind::Set);       \
        ::crd::perf::counter_set_duration(CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__),        \
                                          (value));                                                    \
    } while (false)

// Add kind ------------------------------------------------------------------

#define CRD_PERF_COUNTER_ADD_I64(name_literal, delta)                                                  \
    do                                                                                                 \
    {                                                                                                  \
        static const ::crd::perf::CounterId CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__) =     \
            ::crd::perf::register_counter_i64(name_literal, ::crd::perf::CounterKind::Add);            \
        ::crd::perf::counter_add_i64(CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__), (delta));   \
    } while (false)

#define CRD_PERF_COUNTER_ADD_F64(name_literal, delta)                                                  \
    do                                                                                                 \
    {                                                                                                  \
        static const ::crd::perf::CounterId CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__) =     \
            ::crd::perf::register_counter_f64(name_literal, ::crd::perf::CounterKind::Add);            \
        ::crd::perf::counter_add_f64(CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__), (delta));   \
    } while (false)

#define CRD_PERF_COUNTER_ADD_DURATION(name_literal, delta)                                             \
    do                                                                                                 \
    {                                                                                                  \
        static const ::crd::perf::CounterId CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__) =     \
            ::crd::perf::register_counter_duration(name_literal, ::crd::perf::CounterKind::Add);       \
        ::crd::perf::counter_add_duration(CRD_PERF_COUNTER_DETAIL_CAT(_crd_perf_ci_, __LINE__),        \
                                          (delta));                                                    \
    } while (false)

#else // CRD_PERF_ENABLED == 0

#define CRD_PERF_COUNTER_SET_I64(name_literal, value)       ((void)0)
#define CRD_PERF_COUNTER_SET_F64(name_literal, value)       ((void)0)
#define CRD_PERF_COUNTER_SET_DURATION(name_literal, value)  ((void)0)
#define CRD_PERF_COUNTER_ADD_I64(name_literal, delta)       ((void)0)
#define CRD_PERF_COUNTER_ADD_F64(name_literal, delta)       ((void)0)
#define CRD_PERF_COUNTER_ADD_DURATION(name_literal, delta)  ((void)0)

#endif
