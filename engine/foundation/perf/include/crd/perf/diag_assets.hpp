#pragma once

// crd-perf -- authored diagnostic assets (DIAG.9b): the flight policy and fault plans as versioned TOML documents.
//
// Diagnostic policies are authorable assets on the existing config path (crd-toml), validated while the process is
// healthy and never parsed on a fatal path. Both documents name their kind and schema version first; every other key
// is optional and keeps the in-code default when absent. A key the schema does not know is refused with its name, so
// a misspelt bound can never fall back to a default silently. A value of the wrong type, a negative or oversized
// integer, an unknown trigger or site, and a document that fails the policy's or plan's own validation are refused
// the same way, with the reason naming the key. A refusal leaves `out` untouched.
//
// Flight policy (FlightPolicy, crd/perf/flight_recorder.hpp):
//
//     kind               = "flight-policy"
//     schema_version     = 1
//     capacity           = 1024           # ring events
//     pre_events         = 768            # kept from before the trigger
//     post_events        = 256            # recorded after it at most
//     post_window_ns     = 2000000000     # the post window's time bound (0: events only)
//     hitch_threshold_ns = 50000000       # a frame interval above this is a hitch
//     detail             = "full"         # or "basic": Warning and above only (DIAG.9c)
//     triggers           = ["all"]        # or a list of flight_trigger_name() names
//
// Fault plan (FaultPlan, crd/perf/fault_injection.hpp). Each rule uses the plan text's rule grammar
// "<site>@<first>+<limit>%<ppm>[=<value>]"; the document is assembled into plan text and read by parse_fault_plan,
// so the asset and the fault.arm text can never disagree:
//
//     kind           = "fault-plan"
//     schema_version = 1
//     seed           = 7
//     trigger_flight = true
//     rules          = ["allocation-failure@0+1%1000000", "partial-read@2+1%1000000=16"]
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{
namespace cont = crd::containers;

inline constexpr crd::u32 kDiagAssetSchemaVersion = 1U;
inline constexpr crd::u32 kDiagAssetMaxBytes      = 16U * 1024U; // a larger asset file is refused unread

// Parse a flight-policy document and validate the policy (validate_flight_policy). False with `reason` set on refusal.
[[nodiscard]] bool parse_flight_policy_asset(cont::StringView text, FlightPolicy& out, cont::String& reason);

// Parse a fault-plan document into a validated plan. False with `reason` set on refusal.
[[nodiscard]] bool parse_fault_plan_asset(cont::StringView text, FaultPlan& out, cont::String& reason);

// The canonical flight-policy document for `policy` (every key, triggers by name; "all" when every trigger is armed).
// parse_flight_policy_asset reads it back to the same policy.
void flight_policy_to_asset(const FlightPolicy& policy, cont::String& out);

enum class DiagAssetRead : crd::u8
{
    Ok,
    Missing,  // the file could not be opened
    TooLarge, // the file is larger than kDiagAssetMaxBytes; nothing was read
    Failed,   // a read error
};

// "ok", "missing", "too-large", "failed".
[[nodiscard]] cont::StringView diag_asset_read_name(DiagAssetRead r) noexcept;

// Read an asset file of at most kDiagAssetMaxBytes into `out` (replacing its contents).
[[nodiscard]] DiagAssetRead read_diag_asset_file(const char* path, cont::String& out);

} // namespace crd::perf
