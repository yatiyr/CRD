// crd-perf -- authored diagnostic assets (DIAG.9b). See crd/perf/diag_assets.hpp.

#include <crd/perf/diag_assets.hpp>

#include <crd/toml/toml.hpp>

#include <cstdio>
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
