// crd-perf -- typed diagnostic events, identities and policy (DIAG.2a). See diagnostics.hpp.

#include <crd/perf/diagnostics.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>

#include <chrono>
#include <cstdlib>
#include <mutex>

namespace crd::perf
{
namespace
{
struct CodeRange
{
    cont::String module;
    EventCode    lo = 0U;
    EventCode    hi = 0U;
};

// Small process-wide registry of non-foundation code ranges. std::mutex is a synchronisation
// primitive, not a container -- outside the owning-STL-container ban.
std::mutex&               registry_mutex() noexcept
{
    static std::mutex m;
    return m;
}
cont::Array<CodeRange>&   code_registry()
{
    static cont::Array<CodeRange> ranges;
    return ranges;
}

FatalInvariantHandler g_fatal_handler = nullptr;

// Append a JSON-escaped string body (no surrounding quotes) to `out`.
void append_json_escaped(cont::String& out, cont::StringView s)
{
    for (const char c : s)
    {
        switch (c)
        {
        case '"':  out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n");  break;
        case '\r': out.append("\\r");  break;
        case '\t': out.append("\\t");  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                // Control character -> \u00XX.
                static constexpr char kHex[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xFU]);
                out.push_back(kHex[static_cast<unsigned char>(c) & 0xFU]);
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }
}

void append_u64(cont::String& out, crd::u64 v)
{
    char        buf[20];
    int         n = 0;
    if (v == 0U)
    {
        out.push_back('0');
        return;
    }
    while (v > 0U && n < 20)
    {
        buf[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    while (n-- > 0)
        out.push_back(buf[n]);
}

void append_key_str(cont::String& out, const char* key, cont::StringView val, bool leading_comma)
{
    if (leading_comma)
        out.push_back(',');
    out.push_back('"');
    out.append(key);
    out.append("\":\"");
    append_json_escaped(out, val);
    out.push_back('"');
}

void append_key_num(cont::String& out, const char* key, crd::u64 val, bool leading_comma)
{
    if (leading_comma)
        out.push_back(',');
    out.push_back('"');
    out.append(key);
    out.append("\":");
    append_u64(out, val);
}
} // namespace

cont::StringView severity_name(Severity s) noexcept
{
    switch (s)
    {
    case Severity::Info:               return cont::StringView{"info"};
    case Severity::Warning:            return cont::StringView{"warning"};
    case Severity::RecoverableError:   return cont::StringView{"recoverable_error"};
    case Severity::DeveloperAssertion: return cont::StringView{"developer_assertion"};
    case Severity::InstrumentFailure:  return cont::StringView{"instrument_failure"};
    case Severity::FatalInvariant:     return cont::StringView{"fatal_invariant"};
    }
    return cont::StringView{"unknown"};
}

crd::u64 diagnostic_now_ns() noexcept
{
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<crd::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

bool policy_admits(const DiagnosticPolicy& p, Severity s) noexcept
{
    if (is_fatal(s))
        return true; // required validation is never policy-gated away
    return static_cast<crd::u8>(s) >= static_cast<crd::u8>(p.min_severity);
}

PolicyLoadStatus DiagnosticPolicyStore::load(const DiagnosticPolicy& candidate) noexcept
{
    if (candidate.schema_version != kDiagnosticSchemaVersion)
        return PolicyLoadStatus::RejectedUnknownVersion;

    if (candidate.max_events == 0U || candidate.max_events > kMaxPolicyEvents ||
        candidate.max_message_bytes == 0U || candidate.max_message_bytes > kMaxPolicyMessageBytes)
        return PolicyLoadStatus::RejectedOutOfBounds;

    // Explicit generation-wrap guard: never advance past the max or reuse 0.
    if (m_generation == 0xFFFFFFFFU)
    {
        m_overflowed = true;
        return PolicyLoadStatus::RejectedGenerationWrap;
    }

    m_active = candidate;
    ++m_generation;
    return PolicyLoadStatus::Committed;
}

DiagnosticEvent make_event(EventCode code, Severity sev, SourceIdentity src, cont::StringView message,
                           const DiagnosticPolicyStore& policy, GenerationKey gen)
{
    DiagnosticEvent e;
    e.schema_version = kDiagnosticSchemaVersion;
    e.code           = code;
    e.severity       = sev;
    e.source         = src;
    e.generation     = gen;
    e.timestamp_ns   = diagnostic_now_ns();

    const crd::u32 cap = policy.active().max_message_bytes;
    if (message.size() > cap)
    {
        e.message           = cont::String{message.substr(0, cap)};
        e.message_truncated = true;
    }
    else
    {
        e.message = cont::String{message};
    }
    return e;
}

void to_json(const DiagnosticEvent& e, cont::String& out)
{
    out.push_back('{');
    append_key_num(out, "schema", e.schema_version, false);
    append_key_num(out, "code", e.code, true);
    append_key_str(out, "module", owning_module(e.code), true);
    append_key_str(out, "severity", severity_name(e.severity), true);
    append_key_str(out, "file", e.source.file, true);
    append_key_str(out, "symbol", e.source.symbol, true);
    append_key_num(out, "line", e.source.line, true);
    append_key_num(out, "generation", e.generation.value, true);
    append_key_num(out, "timestamp_ns", e.timestamp_ns, true);
    append_key_str(out, "message", cont::StringView{e.message.c_str(), e.message.size()}, true);
    append_key_num(out, "truncated", e.message_truncated ? 1U : 0U, true);
    out.push_back('}');
}

void to_log_line(const DiagnosticEvent& e, cont::String& out)
{
    out.push_back('[');
    out.append(severity_name(e.severity));
    out.append("] ");
    out.append(owning_module(e.code));
    out.push_back('#');
    append_u64(out, e.code);
    out.append(" (");
    out.append(e.source.file);
    out.push_back(':');
    append_u64(out, e.source.line);
    out.append(") ");
    out.append(cont::StringView{e.message.c_str(), e.message.size()});
    if (e.message_truncated)
        out.append(" [truncated]");
}

bool register_code_range(cont::StringView module_name, EventCode lo, EventCode hi) noexcept
{
    if (lo <= kFoundationCodeMax || hi < lo)
        return false;

    std::lock_guard<std::mutex> guard(registry_mutex());
    cont::Array<CodeRange>&     reg = code_registry();
    for (crd::usize i = 0; i < reg.size(); ++i)
    {
        if (lo <= reg[i].hi && reg[i].lo <= hi) // overlap
            return false;
    }
    CodeRange r;
    r.module = cont::String{module_name};
    r.lo     = lo;
    r.hi     = hi;
    reg.push_back(static_cast<CodeRange&&>(r));
    return true;
}

cont::StringView owning_module(EventCode code) noexcept
{
    if (code <= kFoundationCodeMax)
        return cont::StringView{"foundation"};

    std::lock_guard<std::mutex> guard(registry_mutex());
    const cont::Array<CodeRange>& reg = code_registry();
    for (crd::usize i = 0; i < reg.size(); ++i)
    {
        if (code >= reg[i].lo && code <= reg[i].hi)
            return cont::StringView{reg[i].module.c_str(), reg[i].module.size()};
    }
    return cont::StringView{"unassigned"};
}

void reset_code_registry() noexcept
{
    std::lock_guard<std::mutex> guard(registry_mutex());
    code_registry().clear();
}

void set_fatal_invariant_handler(FatalInvariantHandler handler) noexcept
{
    g_fatal_handler = handler;
}

void check_invariant(bool cond, EventCode code, SourceIdentity src, cont::StringView message)
{
    if (cond)
        return;

    // Build the event with a default (bounded) policy view -- no store is threaded here because a
    // fatal invariant must fire even before any policy is loaded.
    DiagnosticEvent e;
    e.schema_version = kDiagnosticSchemaVersion;
    e.code           = code;
    e.severity       = Severity::FatalInvariant;
    e.source         = src;
    e.timestamp_ns   = diagnostic_now_ns();
    e.message        = cont::String{message};

    if (g_fatal_handler != nullptr)
    {
        g_fatal_handler(e);
        return; // a test handler may choose to continue; production handler does not return
    }

    cont::String line;
    to_log_line(e, line);
    CRD_FATAL(line.c_str());
    std::abort();
}

} // namespace crd::perf
