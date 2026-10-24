// crd-perf -- authored diagnostic assets (DIAG.9b). See crd/perf/diag_assets.hpp.

#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diagnostic_host.hpp>

#include <crd/core/fault_seam.hpp>
#include <crd/time/deadline.hpp>
#include <crd/toml/toml.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace crd::perf
{
namespace
{
namespace toml = crd::toml;

constexpr cont::StringView kFlightPolicyKind{"flight-policy"};
constexpr cont::StringView kFaultPlanKind{"fault-plan"};
constexpr cont::StringView kAllTriggers{"all"};
constexpr crd::u64         kU32Max = 0xFFFFFFFFULL;
constexpr crd::u64         kI64Max = 0x7FFFFFFFFFFFFFFFULL;

void append_decimal(cont::String& out, crd::u64 v)
{
    char     digits[20];
    crd::u32 n = 0U;
    if (v == 0U)
    {
        out.push_back('0');
        return;
    }
    while (v != 0U)
    {
        digits[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0U)
    {
        out.push_back(digits[--n]);
    }
}

void append_signed(cont::String& out, crd::i64 v)
{
    if (v < 0)
    {
        out.push_back('-');
        append_decimal(out, static_cast<crd::u64>(-(v + 1)) + 1U);
        return;
    }
    append_decimal(out, static_cast<crd::u64>(v));
}

// Parse the document; on failure name the TOML error and its position.
bool parse_document(cont::StringView text, toml::parse_result& doc, cont::String& reason)
{
    doc = toml::parse(std::string_view{text.data(), text.size()}, "diagnostic asset");
    if (!doc)
    {
        reason.append("the document is not valid TOML: ");
        reason.append(doc.error().description());
        reason.append(" (line ");
        append_decimal(reason, doc.error().line());
        reason.append(", column ");
        append_decimal(reason, doc.error().column());
        reason.push_back(')');
        return false;
    }
    return true;
}

// The two keys every document leads with: its kind and the schema version this build reads.
bool check_header(const toml::node& root, cont::StringView kind, cont::String& reason)
{
    const toml::node* k = root.get(cont::StringView{"kind"});
    if (k == nullptr)
    {
        reason.append("the document has no kind (expected kind = \"");
        reason.append(kind);
        reason.append("\")");
        return false;
    }
    const auto kind_text = k->value<std::string_view>();
    if (!kind_text.has_value() || cont::StringView{kind_text->data(), kind_text->size()} != kind)
    {
        reason.append("kind: expected \"");
        reason.append(kind);
        reason.append("\"");
        return false;
    }
    const toml::node* v = root.get(cont::StringView{"schema_version"});
    if (v == nullptr)
    {
        reason.append("the document has no schema_version");
        return false;
    }
    if (!v->is_integer() || v->value<crd::i64>().value_or(-1) != static_cast<crd::i64>(kDiagAssetSchemaVersion))
    {
        reason.append("schema_version: this build reads version ");
        append_decimal(reason, kDiagAssetSchemaVersion);
        return false;
    }
    return true;
}

void refuse_key(cont::String& reason, cont::StringView key, const char* why)
{
    reason.append(key);
    reason.append(": ");
    reason.append(why);
}

// An integer key in [0, max]. A float, a negative value or one above `max` is refused with the key named.
bool read_unsigned(const toml::node& value, cont::StringView key, crd::u64 max, crd::u64& out, cont::String& reason)
{
    if (!value.is_integer())
    {
        refuse_key(reason, key, "expected an integer");
        return false;
    }
    const crd::i64 v = value.value<crd::i64>().value_or(-1);
    if (v < 0)
    {
        refuse_key(reason, key, "must not be negative (got ");
        append_signed(reason, v);
        reason.push_back(')');
        return false;
    }
    if (static_cast<crd::u64>(v) > max)
    {
        refuse_key(reason, key, "is larger than ");
        append_decimal(reason, max);
        return false;
    }
    out = static_cast<crd::u64>(v);
    return true;
}

bool read_u32(const toml::node& value, cont::StringView key, crd::u32& out, cont::String& reason)
{
    crd::u64 v = 0U;
    if (!read_unsigned(value, key, kU32Max, v, reason))
    {
        return false;
    }
    out = static_cast<crd::u32>(v);
    return true;
}

bool trigger_by_name(cont::StringView name, crd::u32& bits) noexcept
{
    if (name == kAllTriggers)
    {
        bits = kFlightAllTriggers;
        return true;
    }
    for (crd::u32 t = 1U; t < kFlightTriggerCount; ++t)
    {
        const auto kind = static_cast<FlightTrigger>(t);
        if (name == flight_trigger_name(kind))
        {
            bits = flight_trigger_bit(kind);
            return true;
        }
    }
    return false;
}

bool read_triggers(const toml::node& value, crd::u32& mask, cont::String& reason)
{
    if (!value.is_array())
    {
        refuse_key(reason, cont::StringView{"triggers"}, "expected an array of trigger names");
        return false;
    }
    crd::u32 bits = 0U;
    for (const toml::node& element : value)
    {
        const auto name = element.value<std::string_view>();
        if (!name.has_value())
        {
            refuse_key(reason, cont::StringView{"triggers"}, "every element must be a trigger name");
            return false;
        }
        crd::u32 one = 0U;
        if (!trigger_by_name(cont::StringView{name->data(), name->size()}, one))
        {
            refuse_key(reason, cont::StringView{"triggers"}, "unknown trigger \"");
            reason.append(cont::StringView{name->data(), name->size()});
            reason.append(R"(" (expected "all" or a flight trigger name))");
            return false;
        }
        bits |= one;
    }
    mask = bits;
    return true;
}

bool read_detail(const toml::node& value, FlightDetail& detail, cont::String& reason)
{
    const auto name = value.value<std::string_view>();
    if (!name.has_value())
    {
        refuse_key(reason, cont::StringView{"detail"}, R"(expected "full" or "basic")");
        return false;
    }
    const cont::StringView text{name->data(), name->size()};
    for (const FlightDetail d : {FlightDetail::Full, FlightDetail::Basic})
    {
        if (text == flight_detail_name(d))
        {
            detail = d;
            return true;
        }
    }
    refuse_key(reason, cont::StringView{"detail"}, R"(unknown detail level ")");
    reason.append(text);
    reason.append(R"(" (expected "full" or "basic"))");
    return false;
}

bool is_header_key(cont::StringView key) noexcept
{
    return key == cont::StringView{"kind"} || key == cont::StringView{"schema_version"};
}

void refuse_unknown(cont::String& reason, cont::StringView key, cont::StringView kind)
{
    reason.append("unknown key \"");
    reason.append(key);
    reason.append("\" in a ");
    reason.append(kind);
    reason.append(" document");
}
} // namespace

bool parse_flight_policy_asset(cont::StringView text, FlightPolicy& out, cont::String& reason)
{
    toml::parse_result doc;
    if (!parse_document(text, doc, reason))
    {
        return false;
    }
    const toml::node& root = doc.table();
    if (!check_header(root, kFlightPolicyKind, reason))
    {
        return false;
    }

    FlightPolicy p;
    for (auto&& [key, value] : root.items())
    {
        bool ok = true;
        if (is_header_key(key))
        {
            continue;
        }
        if (key == cont::StringView{"capacity"})
        {
            ok = read_u32(value, key, p.capacity, reason);
        }
        else if (key == cont::StringView{"pre_events"})
        {
            ok = read_u32(value, key, p.pre_events, reason);
        }
        else if (key == cont::StringView{"post_events"})
        {
            ok = read_u32(value, key, p.post_events, reason);
        }
        else if (key == cont::StringView{"post_window_ns"})
        {
            ok = read_unsigned(value, key, kI64Max, p.post_window_ns, reason);
        }
        else if (key == cont::StringView{"hitch_threshold_ns"})
        {
            ok = read_unsigned(value, key, kI64Max, p.hitch_threshold_ns, reason);
        }
        else if (key == cont::StringView{"triggers"})
        {
            ok = read_triggers(value, p.trigger_mask, reason);
        }
        else if (key == cont::StringView{"detail"})
        {
            ok = read_detail(value, p.detail, reason);
        }
        else
        {
            refuse_unknown(reason, key, kFlightPolicyKind);
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
    }

    const FlightPolicyStatus status = validate_flight_policy(p);
    if (status != FlightPolicyStatus::Committed)
    {
        reason.append("the policy is refused: ");
        reason.append(flight_policy_status_name(status));
        reason.append(" (capacity 2 to 65536; pre_events and post_events at least 1 with pre_events + post_events "
                      "<= capacity; post_window_ns at most 60 s; hitch_threshold_ns and triggers not empty)");
        return false;
    }
    out = p;
    return true;
}

bool parse_fault_plan_asset(cont::StringView text, FaultPlan& out, cont::String& reason)
{
    toml::parse_result doc;
    if (!parse_document(text, doc, reason))
    {
        return false;
    }
    const toml::node& root = doc.table();
    if (!check_header(root, kFaultPlanKind, reason))
    {
        return false;
    }

    crd::u64          seed           = 0U;
    bool              trigger_flight = true;
    const toml::node* rules          = nullptr;
    for (auto&& [key, value] : root.items())
    {
        if (is_header_key(key))
        {
            continue;
        }
        if (key == cont::StringView{"seed"})
        {
            if (!read_unsigned(value, key, kI64Max, seed, reason))
            {
                return false;
            }
        }
        else if (key == cont::StringView{"trigger_flight"})
        {
            if (!value.is_boolean())
            {
                refuse_key(reason, key, "expected true or false");
                return false;
            }
            trigger_flight = value.value<bool>().value_or(true);
        }
        else if (key == cont::StringView{"rules"})
        {
            if (!value.is_array())
            {
                refuse_key(reason, key, "expected an array of rules");
                return false;
            }
            rules = &value;
        }
        else
        {
            refuse_unknown(reason, key, kFaultPlanKind);
            return false;
        }
    }

    // Assemble the plan text; parse_fault_plan is the one reader of the rule grammar.
    cont::String plan_text;
    plan_text.append("seed=");
    append_decimal(plan_text, seed);
    if (!trigger_flight)
    {
        plan_text.append(";notrigger");
    }
    if (rules != nullptr)
    {
        for (const toml::node& element : *rules)
        {
            const auto rule = element.value<std::string_view>();
            if (!rule.has_value())
            {
                refuse_key(reason, cont::StringView{"rules"}, "every element must be a rule string");
                return false;
            }
            const cont::StringView view{rule->data(), rule->size()};
            // A rule is one field of the plan text: a ';' would smuggle in another field, and a rule always names its
            // first hit with '@' (so a bare "notrigger" can never become the plan's flag).
            if (view.find(';') != cont::StringView::npos || view.find('@') == cont::StringView::npos)
            {
                refuse_key(reason, cont::StringView{"rules"}, "a rule is <site>@<first>+<limit>%<ppm>[=<value>]: ");
                reason.append(view);
                return false;
            }
            plan_text.push_back(';');
            plan_text.append(view);
        }
    }
    FaultPlan plan;
    if (!parse_fault_plan(cont::StringView{plan_text.data(), plan_text.size()}, plan, reason))
    {
        return false;
    }
    out = plan;
    return true;
}

void flight_policy_to_asset(const FlightPolicy& policy, cont::String& out)
{
    out.clear();
    out.append("kind = \"flight-policy\"\nschema_version = ");
    append_decimal(out, policy.schema_version);
    out.append("\ncapacity = ");
    append_decimal(out, policy.capacity);
    out.append("\npre_events = ");
    append_decimal(out, policy.pre_events);
    out.append("\npost_events = ");
    append_decimal(out, policy.post_events);
    out.append("\npost_window_ns = ");
    append_decimal(out, policy.post_window_ns);
    out.append("\nhitch_threshold_ns = ");
    append_decimal(out, policy.hitch_threshold_ns);
    out.append("\ndetail = \"");
    out.append(flight_detail_name(policy.detail));
    out.append("\"\ntriggers = [");
    if (policy.trigger_mask == kFlightAllTriggers)
    {
        out.append("\"all\"");
    }
    else
    {
        bool first = true;
        for (crd::u32 t = 1U; t < kFlightTriggerCount; ++t)
        {
            const auto kind = static_cast<FlightTrigger>(t);
            if ((policy.trigger_mask & flight_trigger_bit(kind)) == 0U)
            {
                continue;
            }
            if (!first)
            {
                out.append(", ");
            }
            first = false;
            out.push_back('"');
            out.append(flight_trigger_name(kind));
            out.push_back('"');
        }
    }
    out.append("]\n");
}

// ---- The diagnostic-host profile (DIAG.11a) ----------------------------------------------------------------------

namespace
{
constexpr cont::StringView kDiagHostKind{"diagnostic-host"};
constexpr crd::usize       kDiagHostMaxPathBytes = 1024U;
constexpr DiagAuthoritySet kRemoteAuthorities    = authority_bit(DiagAuthority::RemoteEnable) |
                                                authority_bit(DiagAuthority::Upload) |
                                                authority_bit(DiagAuthority::ProcessMemory);

bool read_mode(const toml::node& value, cont::StringView key, DiagMode& out, cont::String& reason)
{
    const auto name = value.value<std::string_view>();
    if (name.has_value())
    {
        const cont::StringView text{name->data(), name->size()};
        for (const DiagMode m : {DiagMode::Off, DiagMode::On, DiagMode::Required})
        {
            if (text == diag_mode_name(m))
            {
                out = m;
                return true;
            }
        }
    }
    refuse_key(reason, key, R"(expected "off", "on" or "required")");
    return false;
}

bool read_path(const toml::node& value, cont::StringView key, cont::String& out, cont::String& reason)
{
    const auto text = value.value<std::string_view>();
    if (!text.has_value())
    {
        refuse_key(reason, key, "expected a path string");
        return false;
    }
    out.clear();
    out.append(cont::StringView{text->data(), text->size()});
    return true;
}

bool read_grant(const toml::node& value, DiagAuthoritySet& out, cont::String& reason)
{
    const cont::StringView key{"command_grant"};
    if (!value.is_array())
    {
        refuse_key(reason, key, "expected an array of authority names");
        return false;
    }
    DiagAuthoritySet set = 0U;
    for (const toml::node& element : value)
    {
        const auto name = element.value<std::string_view>();
        if (!name.has_value())
        {
            refuse_key(reason, key, "every element must be an authority name");
            return false;
        }
        const cont::StringView text{name->data(), name->size()};
        bool                   known = false;
        for (crd::u32 bit = 0U; bit < kDiagAuthorityCount && !known; ++bit)
        {
            const auto a = static_cast<DiagAuthority>(1U << bit);
            if (text == authority_name(a))
            {
                set |= authority_bit(a);
                known = true;
            }
        }
        if (!known)
        {
            refuse_key(reason, key, "unknown authority \"");
            reason.append(text);
            reason.append(R"(" (expected read, record, inject, remote-enable, upload, process-memory or execute))");
            return false;
        }
    }
    out = set;
    return true;
}

// A TOML basic string of `text`: quotes, backslashes and control characters escaped.
void append_toml_string(cont::String& out, cont::StringView text)
{
    out.push_back('"');
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (byte < 0x20U || byte == 0x7FU)
        {
            char escape[8];
            (void)std::snprintf(escape, sizeof(escape), "\\u%04X", static_cast<unsigned>(byte));
            out.append(escape);
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

bool check_path_bytes(const cont::String& path, cont::StringView key, cont::String& reason)
{
    if (path.size() > kDiagHostMaxPathBytes)
    {
        refuse_key(reason, key, "is longer than 1024 bytes");
        return false;
    }
    if (cont::StringView{path.data(), path.size()}.find('\0') != cont::StringView::npos)
    {
        refuse_key(reason, key, "must not contain a NUL character");
        return false;
    }
    return true;
}
} // namespace

bool validate_diagnostic_host_profile(const DiagnosticHostProfile& p, cont::String& reason)
{
    if (!check_path_bytes(p.crash_dir, cont::StringView{"crash_dir"}, reason) ||
        !check_path_bytes(p.flight_policy, cont::StringView{"flight_policy"}, reason) ||
        !check_path_bytes(p.capture_dir, cont::StringView{"capture_dir"}, reason))
    {
        return false;
    }
    if (p.crash_handler != DiagMode::Off && p.crash_dir.empty())
    {
        refuse_key(reason, cont::StringView{"crash_dir"}, "must name a directory while the crash handler is asked for");
        return false;
    }
    if (p.flight == DiagMode::Off && !p.flight_policy.empty())
    {
        refuse_key(reason, cont::StringView{"flight_policy"}, "needs flight on or required");
        return false;
    }
    if (p.flight == DiagMode::Off && !p.capture_dir.empty())
    {
        refuse_key(reason, cont::StringView{"capture_dir"}, "flight bundles need flight on or required");
        return false;
    }
    if (p.flight == DiagMode::Off && p.fault_injection != DiagMode::Off)
    {
        refuse_key(reason, cont::StringView{"fault_injection"}, "needs flight on or required (a fault is a trigger)");
        return false;
    }
    if (p.max_bundles == 0U || p.max_bundles > kDiagHostMaxBundles)
    {
        refuse_key(reason, cont::StringView{"max_bundles"}, "must be 1 to 1000");
        return false;
    }
    if (p.watchdog_period_ms > kDiagHostMaxWatchdogMs)
    {
        refuse_key(reason, cont::StringView{"watchdog_period_ms"}, "is larger than 60000");
        return false;
    }
    if ((p.command_grant & ~kDiagAllAuthorities) != 0U)
    {
        refuse_key(reason, cont::StringView{"command_grant"}, "names an authority this build does not know");
        return false;
    }
    if ((p.command_grant & kRemoteAuthorities) != 0U && !p.allow_remote)
    {
        refuse_key(reason, cont::StringView{"command_grant"},
                   "remote-enable, upload and process-memory need allow_remote = true");
        return false;
    }
    if (grants(p.command_grant, DiagAuthority::Inject) && p.fault_injection == DiagMode::Off)
    {
        refuse_key(reason, cont::StringView{"command_grant"}, "inject needs fault_injection on or required");
        return false;
    }
    return true;
}

bool parse_diagnostic_host_profile(cont::StringView text, DiagnosticHostProfile& out, cont::String& reason)
{
    toml::parse_result doc;
    if (!parse_document(text, doc, reason))
    {
        return false;
    }
    const toml::node& root = doc.table();
    if (!check_header(root, kDiagHostKind, reason))
    {
        return false;
    }

    DiagnosticHostProfile p;
    for (auto&& [key, value] : root.items())
    {
        bool ok = true;
        if (is_header_key(key))
        {
            continue;
        }
        if (key == cont::StringView{"crash_handler"})
        {
            ok = read_mode(value, key, p.crash_handler, reason);
        }
        else if (key == cont::StringView{"crash_dir"})
        {
            ok = read_path(value, key, p.crash_dir, reason);
        }
        else if (key == cont::StringView{"flight"})
        {
            ok = read_mode(value, key, p.flight, reason);
        }
        else if (key == cont::StringView{"flight_policy"})
        {
            ok = read_path(value, key, p.flight_policy, reason);
        }
        else if (key == cont::StringView{"capture_dir"})
        {
            ok = read_path(value, key, p.capture_dir, reason);
        }
        else if (key == cont::StringView{"max_bundles"})
        {
            ok = read_u32(value, key, p.max_bundles, reason);
        }
        else if (key == cont::StringView{"fault_injection"})
        {
            ok = read_mode(value, key, p.fault_injection, reason);
        }
        else if (key == cont::StringView{"command_grant"})
        {
            ok = read_grant(value, p.command_grant, reason);
        }
        else if (key == cont::StringView{"allow_remote"})
        {
            if (!value.is_boolean())
            {
                refuse_key(reason, key, "expected true or false");
                ok = false;
            }
            else
            {
                p.allow_remote = value.value<bool>().value_or(false);
            }
        }
        else if (key == cont::StringView{"watchdog_period_ms"})
        {
            ok = read_u32(value, key, p.watchdog_period_ms, reason);
        }
        else
        {
            refuse_unknown(reason, key, kDiagHostKind);
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
    }
    if (!validate_diagnostic_host_profile(p, reason))
    {
        return false;
    }
    out = p;
    return true;
}

bool load_diagnostic_host_profile(const char* path, DiagnosticHostProfile& out, cont::String& reason)
{
    cont::String        text;
    const DiagAssetRead read = path != nullptr ? read_diag_asset_file(path, text) : DiagAssetRead::Missing;
    if (read != DiagAssetRead::Ok)
    {
        reason.append("the diagnostic-host profile could not be read: ");
        reason.append(diag_asset_read_name(read));
        return false;
    }
    return parse_diagnostic_host_profile(cont::StringView{text.data(), text.size()}, out, reason);
}

void diagnostic_host_profile_to_asset(const DiagnosticHostProfile& p, cont::String& out)
{
    out.clear();
    out.append("kind = \"diagnostic-host\"\nschema_version = ");
    append_decimal(out, kDiagAssetSchemaVersion);
    out.append("\ncrash_handler = \"");
    out.append(diag_mode_name(p.crash_handler));
    out.append("\"\ncrash_dir = ");
    append_toml_string(out, cont::StringView{p.crash_dir.data(), p.crash_dir.size()});
    out.append("\nflight = \"");
    out.append(diag_mode_name(p.flight));
    out.append("\"\nflight_policy = ");
    append_toml_string(out, cont::StringView{p.flight_policy.data(), p.flight_policy.size()});
    out.append("\ncapture_dir = ");
    append_toml_string(out, cont::StringView{p.capture_dir.data(), p.capture_dir.size()});
    out.append("\nmax_bundles = ");
    append_decimal(out, p.max_bundles);
    out.append("\nfault_injection = \"");
    out.append(diag_mode_name(p.fault_injection));
    out.append("\"\ncommand_grant = [");
    bool first = true;
    for (crd::u32 bit = 0U; bit < kDiagAuthorityCount; ++bit)
    {
        const auto a = static_cast<DiagAuthority>(1U << bit);
        if (!grants(p.command_grant, a))
        {
            continue;
        }
        if (!first)
        {
            out.append(", ");
        }
        first = false;
        out.push_back('"');
        out.append(authority_name(a));
        out.push_back('"');
    }
    out.append("]\nallow_remote = ");
    out.append(p.allow_remote ? "true" : "false");
    out.append("\nwatchdog_period_ms = ");
    append_decimal(out, p.watchdog_period_ms);
    out.push_back('\n');
}

cont::StringView diag_asset_read_name(DiagAssetRead r) noexcept
{
    switch (r) // no default: every result is named
    {
    case DiagAssetRead::Ok:       return cont::StringView{"ok"};
    case DiagAssetRead::Missing:  return cont::StringView{"missing"};
    case DiagAssetRead::TooLarge: return cont::StringView{"too-large"};
    case DiagAssetRead::Failed:   return cont::StringView{"failed"};
    }
    return cont::StringView{"unknown"};
}

DiagAssetRead read_diag_asset_file(const char* path, cont::String& out)
{
    out.clear();
    // The I/O fault seam, consulted before the file is touched as crd-platform's whole-file reads do: a failed, short
    // or cancelled read is Failed, and a delay is slept here.
    const crd::IoFault fault = crd::consult_io_fault(crd::IoFaultOp::ReadWhole, path, std::strlen(path), 0U, 0U);
    if (fault.action == crd::IoFaultAction::Fail || fault.action == crd::IoFaultAction::Short ||
        fault.action == crd::IoFaultAction::Cancel)
    {
        return DiagAssetRead::Failed;
    }
    if (fault.action == crd::IoFaultAction::Delay)
    {
        crd::time::sleep_for(crd::time::Duration{static_cast<crd::f64>(fault.value) * 1.0e-9});
    }
    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path, "rb") != 0)
    {
        fp = nullptr;
    }
#else
    fp = std::fopen(path, "rb");
#endif
    if (fp == nullptr)
    {
        return DiagAssetRead::Missing;
    }
    // Read at most one byte past the bound: a file that has it is refused, and nothing further is read.
    char          chunk[1024];
    crd::u64      total  = 0U;
    DiagAssetRead result = DiagAssetRead::Ok;
    for (;;)
    {
        const std::size_t got = std::fread(chunk, 1U, sizeof(chunk), fp);
        if (got == 0U)
        {
            if (std::ferror(fp) != 0)
            {
                result = DiagAssetRead::Failed;
            }
            break;
        }
        total += got;
        if (total > kDiagAssetMaxBytes)
        {
            result = DiagAssetRead::TooLarge;
            break;
        }
        out.append(cont::StringView{chunk, got});
    }
    (void)std::fclose(fp);
    if (result != DiagAssetRead::Ok)
    {
        out.clear();
    }
    return result;
}

} // namespace crd::perf
