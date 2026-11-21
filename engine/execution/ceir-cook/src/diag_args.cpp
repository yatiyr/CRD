#include "diag_args.hpp"

#include <crd/ceir/cook/replay_diag.hpp> // HostClockSpec
#include <crd/perf/diag_commands.hpp>

#include <algorithm>

namespace crd::ceir::cook::detail
{
bool valid_entry_name(containers::StringView name, crd::u32 max_bytes) noexcept
{
    if (name.empty() || name.size() > max_bytes)
    {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                          c == '_' || c == '.';
                               });
}

void clock_fields(perf::DiagFields& fields, const HostClockSpec& spec)
{
    fields.str("wall_clock", spec.live_wall ? containers::StringView{"live"} : containers::StringView{"none"})
        .str("sim_time", spec.has_sim_time ? containers::StringView{"set"} : containers::StringView{"none"})
        .i64("sim_time_ns", spec.sim_time)
        .str("sim_step", spec.has_sim_step ? containers::StringView{"set"} : containers::StringView{"none"})
        .i64("sim_step_ns", spec.sim_step);
}

void event_fields(perf::DiagFields& fields, const HostEventsSpec& spec)
{
    fields.str("event_queue", spec.open ? containers::StringView{"open"} : containers::StringView{"none"})
        .u64("input_events", spec.events.size());
}
} // namespace crd::ceir::cook::detail
