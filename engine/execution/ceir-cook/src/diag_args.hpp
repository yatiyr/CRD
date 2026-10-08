#pragma once

// crd-ceir-cook (private) -- the argument parsers and answer fields the program diagnostic commands share. Each
// parser parses one named argument's value as the command's check and handler both see it; none allocates or touches
// a file.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{
class DiagFields;
} // namespace crd::perf

namespace crd::ceir::cook
{
struct HostClockSpec;
struct HostEventsSpec;
} // namespace crd::ceir::cook

namespace crd::ceir::cook::detail
{
// Call `item` on every comma-separated item of `list` (none for an empty list); false as soon as one is refused.
template <class Fn>
[[nodiscard]] bool for_each_item(containers::StringView list, const Fn& item)
{
    if (list.empty())
    {
        return true;
    }
    crd::usize start = 0U;
    while (start <= list.size())
    {
        crd::usize end = list.find(',', start);
        if (end == containers::StringView::npos)
        {
            end = list.size();
        }
        if (!item(list.substr(start, end - start)))
        {
            return false;
        }
        start = end + 1U;
    }
    return true;
}

// A decimal in [0, max] (1 to 20 digits, no sign).
[[nodiscard]] bool parse_u64(containers::StringView text, crd::u64 max, crd::u64& out) noexcept;

// A decimal i64 with an optional leading '-'.
[[nodiscard]] bool parse_i64(containers::StringView text, crd::i64& out) noexcept;

// An entry function name: 1 to `max_bytes` bytes of [A-Za-z0-9_.].
[[nodiscard]] bool valid_entry_name(containers::StringView name, crd::u32 max_bytes) noexcept;

// The clock a run was given, as every command's answer names it: `wall_clock` (live or none), `sim_time` and
// `sim_step` (set or none) with their nanoseconds.
void clock_fields(perf::DiagFields& fields, const HostClockSpec& spec);

// The input event queue a run was given: `event_queue` (open or none) and `input_events`, the events it held.
void event_fields(perf::DiagFields& fields, const HostEventsSpec& spec);
} // namespace crd::ceir::cook::detail
