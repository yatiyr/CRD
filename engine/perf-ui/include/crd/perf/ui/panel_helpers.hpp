#pragma once

// ---------------------------------------------------------------------------
// crd-perf-ui -- panel rendering helpers (Detour D-003 v0g).
//
// Small, testable utilities consumed by every panel:
//
//   - color_for_name(NameId)          stable visual color per region name
//   - format_duration(ns)             "16.667 ms" / "120 us" / "750 ns"
//   - format_bytes(u64)               "16.4 MB" / "780 KB" / "512 B"
//   - format_count(u64)               "1.2k" / "850" / "4.5M"
//
// All functions are deterministic + side-effect-free; covered by unit
// tests in tests/perf-ui/.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/sample.hpp>

namespace crd::perf::ui
{

// Packed RGBA color (premultiplied is fine; ImGui treats this as the
// IM_COL32 layout when reinterpreted).
struct Color32
{
    crd::u32 value = 0xFFFFFFFFU;
};

// Map a NameId to a deterministic, evenly-distributed color. Same NameId
// across separate runs yields the same color so users build muscle memory.
[[nodiscard]] Color32 color_for_name(NameId name) noexcept;

// Map a Category to a default color used when a Sample has color_rgba == 0.
[[nodiscard]] Color32 color_for_category(Category cat) noexcept;

// Format a duration in nanoseconds. Picks the largest scale that yields
// >= 1.0. Writes "fits-in-32-chars" output to `buf`; returns strlen.
//
//   1            -> "1 ns"
//   1500         -> "1.500 us"
//   2'500'000    -> "2.500 ms"
//   1'500'000'000-> "1.500 s"
//   60'000'000'000 -> "60.000 s"
crd::usize format_duration(crd::i64 ns, char* buf, crd::usize buf_size) noexcept;

// Format a byte count with binary multipliers (KiB / MiB / GiB but using
// 1024 multipliers and "KB / MB / GB" labels per common-tool convention).
crd::usize format_bytes(crd::u64 bytes, char* buf, crd::usize buf_size) noexcept;

// Format a generic count with 'k' / 'M' / 'B' shorthand for readability.
crd::usize format_count(crd::u64 count, char* buf, crd::usize buf_size) noexcept;

// Sum the durations (end_ns - begin_ns) for all samples on a thread.
[[nodiscard]] crd::u64 total_thread_duration_ns(
    crd::containers::ConstSpan<Sample> samples) noexcept;

// Compute self-time per top-level NameId on a thread. Top-level == depth 0.
// Writes into the caller's two parallel arrays; returns the count of distinct
// names found. `out_capacity` is the max names to emit.
//
// Result is unsorted; callers sort by self_ns descending if they want top-N.
struct NameTotal
{
    NameId   name;
    crd::u64 total_ns;
    crd::u32 occurrences;
};

crd::u32 aggregate_top_level_by_name(crd::containers::ConstSpan<Sample> samples,
                                      NameTotal* out_totals,
                                      crd::u32 out_capacity) noexcept;

} // namespace crd::perf::ui
