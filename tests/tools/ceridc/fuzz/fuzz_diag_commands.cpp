// crd-fuzz-ceridc-diag: command decoding -- the MCP transport ceridc serves (mcp_handle: JSON-RPC 2.0 over the owned
// JSON parser), its diag tool's argument decoding (call_diag) and the typed diagnostic command service beneath it
// (DiagCommandService::execute and every registered command's argument check) -- under the bounded fuzz harness
// (DIAG.10a; docs/design/test-instruments.md).
//
// An input is a host line and up to 64 request lines:
//   #host <grant>[ root]   optional first line: the host's grant (a parse_authority_list text; "read" otherwise) and
//                          whether it has a file root. The root is a directory that does not exist, so every path
//                          command passes the service's path checks and reaches its argument check and handler, and
//                          every file it opens fails: nothing is read or written.
//   @<json>                the diag tool's `arguments` value, spliced into a tools/call request with a fixed envelope
//   !<fields>              a native request, '|'-separated: command|path|cursor|page_items|page_bytes|schema|name=value
//                          ... (raw bytes; an empty number field is the default, digits saturate)
//   <json>                 one JSON-RPC line, fed to mcp_handle as the stdio transport would
// One service per input: the host's grant and root, ceridc's commands (bind_diag_commands) and the flight and fault
// commands of a small recorder and an injector (the sandbox's), so every argument check of both hosts is reachable.
// A line whose method could be tools/call and whose tool could be one of ceridc's file verbs (import, cook, query,
// instantiate, sequence, render, export) is never fed: they read and write paths the input names.
//
// Oracles, none of them the code under test's own verdict:
//   * every line the owned parser accepts is RFC 8259 text with well-formed UTF-8 and within its documented limits
//     (64 levels of nesting, numbers a double holds), and every such text is accepted: an independent validator below;
//   * every reply is one line of valid UTF-8 JSON, -32700 for an unparsed line, -32600 for a request that is not an
//     object or whose id cannot be echoed (null id) or whose method is not a string, no reply to a notification, and
//     otherwise the request's own id, a string as the same string and an integer as the same integer;
//   * the diag tool decodes exactly the documented fields (command a string; path a string; cursor, page_items,
//     page_bytes and schema whole numbers to 2^53 - 1, clamped to u32 where the field is; args an object of strings)
//     or answers -32602 without the service running anything, and the report it returns is the service's document
//     byte for byte: the same request sent natively for the page the report names returns the same bytes;
//   * the service refuses in the documented order (schema, command, authority, request bounds, arguments, path, root,
//     argument names, the command's own check, cursor) before any work, a refusal leaves its handler unrun, and an
//     argument check answers only bad-argument or unavailable;
//   * every service document is valid UTF-8 JSON whose fields equal the typed result, echoes the command clipped at a
//     sequence boundary, names the grant, has a reason exactly when it refuses, keeps every item within 1000 bytes
//     and every page within its item and byte bounds; paging the snapshot to its end covers it once, each page makes
//     progress, a byte-bounded page could not take its next item, and the same cursor returns the same bytes;
//   * the run returns every byte it allocated.
// The commands' file readers open files directly (not through crd-core's I/O fault seam), so which paths they open is
// not observed here; the root does not exist, so none of them opens anything.
#include <crd/fuzz/harness.hpp>

#include <crd/assetio/json.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_commands.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagArg;
using crd::perf::DiagAuthority;
using crd::perf::DiagAuthoritySet;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagStatus;
namespace json = crd::assetio::json;
namespace perf = crd::perf;

// The host's file root: a directory that must not exist (checked before the first input).
constexpr StringView kAbsentRoot{"crd-fuzz-ceridc-diag-absent-root"};
constexpr usize      kMaxLines     = 64U;
constexpr usize      kMaxNativeArgs = 16U;
constexpr u32        kMaxWalkPages = 512U;
constexpr u32        kMaxDepth     = 64U; // the owned parser's documented nesting limit
constexpr double     kMaxExact     = 9007199254740991.0;
constexpr u32        kNone         = 0xFFFFFFFFU;

[[nodiscard]] ConstSpan<u8> bytes_of(StringView s) noexcept
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(s.data()), s.size());
}

[[nodiscard]] StringView view(const String& s) noexcept
{
    return StringView{s.data(), s.size()};
}

// ---- UTF-8, restated: decode a code point and check its range (not crd::utf8_sequence_length's table) -------------

// The length of the well-formed sequence at `p` and its code point, or 0.
[[nodiscard]] usize decode_utf8(const u8* p, usize n, u32& cp) noexcept
{
    const u8 b      = p[0];
    usize    length = 0U;
    u32      least  = 0U;
    if (b < 0x80U)
    {
        cp = b;
        return 1U;
    }
    if ((b & 0xE0U) == 0xC0U)
    {
        length = 2U;
        cp     = b & 0x1FU;
        least  = 0x80U;
    }
    else if ((b & 0xF0U) == 0xE0U)
    {
        length = 3U;
        cp     = b & 0x0FU;
        least  = 0x800U;
    }
    else if ((b & 0xF8U) == 0xF0U)
    {
        length = 4U;
        cp     = b & 0x07U;
        least  = 0x10000U;
    }
    else
    {
        return 0U;
    }
    if (n < length)
    {
        return 0U;
    }
    for (usize i = 1U; i < length; ++i)
    {
        if ((p[i] & 0xC0U) != 0x80U)
        {
            return 0U;
        }
        cp = (cp << 6U) | (p[i] & 0x3FU);
    }
    if (cp < least || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU))
    {
        return 0U;
    }
    return length;
}

// What a JSON writer must make of `s`: each well-formed sequence kept, each other byte U+FFFD.
void sanitized(StringView s, String& out)
{
    const auto* p = reinterpret_cast<const u8*>(s.data());
    for (usize i = 0U; i < s.size();)
    {
        u32         cp     = 0U;
        const usize length = decode_utf8(p + i, s.size() - i, cp);
        if (length == 0U)
        {
            out.append(StringView{"\xEF\xBF\xBD"});
            ++i;
        }
        else
        {
            out.append(StringView{s.data() + i, length});
            i += length;
        }
    }
}

// ---- an independent RFC 8259 validator (iterative: a 64 KiB input of '[' must not exhaust the harness's stack) ----

struct Validation
{
    bool valid            = false;
    u32  max_depth        = 0U;   // the deepest value; the root is at 0
    bool numbers_in_range = true; // every number is one a double holds
    bool raw_line_break   = false;
};

class Validator
{
public:
    Validator(const u8* data, usize size, crd::memory::IAllocator* alloc) : m_p(data), m_end(data + size), m_stack(alloc)
    {
        for (usize i = 0U; i < size; ++i)
        {
            m_result.raw_line_break = m_result.raw_line_break || data[i] == '\n' || data[i] == '\r';
        }
    }

    [[nodiscard]] Validation run()
    {
        m_result.valid = document();
        return m_result;
    }

private:
    [[nodiscard]] static bool is_digit(u8 c) noexcept { return c >= '0' && c <= '9'; }

    void ws() noexcept
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
        {
            ++m_p;
        }
    }

    [[nodiscard]] bool eat(u8 c) noexcept
    {
        if (m_p < m_end && *m_p == c)
        {
            ++m_p;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool hex4(u32& out) noexcept
    {
        out = 0U;
        for (int i = 0; i < 4; ++i)
        {
            if (m_p >= m_end)
            {
                return false;
            }
            const u8 c = *m_p++;
            u32      d = 0U;
            if (is_digit(c))
            {
                d = c - u32{'0'};
            }
            else if (c >= 'a' && c <= 'f')
            {
                d = 10U + (c - u32{'a'});
            }
            else if (c >= 'A' && c <= 'F')
            {
                d = 10U + (c - u32{'A'});
            }
            else
            {
                return false;
            }
            out = (out << 4U) | d;
        }
        return true;
    }

    // After the opening quote. Unicode text only: a \u escape of a surrogate must be a high-low pair.
    [[nodiscard]] bool string() noexcept
    {
        for (;;)
        {
            if (m_p >= m_end)
            {
                return false;
            }
            const u8 c = *m_p;
            if (c == '"')
            {
                ++m_p;
                return true;
            }
            if (c < 0x20U)
            {
                return false;
            }
            if (c == '\\')
            {
                ++m_p;
                if (m_p >= m_end)
                {
                    return false;
                }
                const u8 e = *m_p++;
                if (e == 'u')
                {
                    u32 unit = 0U;
                    if (!hex4(unit) || (unit >= 0xDC00U && unit <= 0xDFFFU))
                    {
                        return false;
                    }
                    if (unit >= 0xD800U && unit <= 0xDBFFU)
                    {
                        u32 low = 0U;
                        if (!eat('\\') || !eat('u') || !hex4(low) || low < 0xDC00U || low > 0xDFFFU)
                        {
                            return false;
                        }
                    }
                }
                else if (std::strchr("\"\\/bfnrt", static_cast<int>(e)) == nullptr || e == 0U)
                {
                    return false;
                }
                continue;
            }
            u32         cp     = 0U;
            const usize length = decode_utf8(m_p, static_cast<usize>(m_end - m_p), cp);
            if (length == 0U)
            {
                return false;
            }
            m_p += length;
        }
    }

    [[nodiscard]] bool number() noexcept
    {
        const u8* const first = m_p;
        (void)eat('-');
        if (!eat('0'))
        {
            if (m_p >= m_end || *m_p < '1' || *m_p > '9')
            {
                return false;
            }
            while (m_p < m_end && is_digit(*m_p))
            {
                ++m_p;
            }
        }
        if (eat('.'))
        {
            if (m_p >= m_end || !is_digit(*m_p))
            {
                return false;
            }
            while (m_p < m_end && is_digit(*m_p))
            {
                ++m_p;
            }
        }
        if (m_p < m_end && (*m_p == 'e' || *m_p == 'E'))
        {
            ++m_p;
            if (!eat('+'))
            {
                (void)eat('-');
            }
            if (m_p >= m_end || !is_digit(*m_p))
            {
                return false;
            }
            while (m_p < m_end && is_digit(*m_p))
            {
                ++m_p;
            }
        }
        double     value = 0.0;
        const auto res   = std::from_chars(reinterpret_cast<const char*>(first), reinterpret_cast<const char*>(m_p),
                                           value, std::chars_format::general);
        if (res.ec != std::errc{})
        {
            m_result.numbers_in_range = false;
        }
        return true;
    }

    [[nodiscard]] bool literal(const char* text) noexcept
    {
        const usize n = std::strlen(text);
        if (static_cast<usize>(m_end - m_p) < n || std::memcmp(m_p, text, n) != 0)
        {
            return false;
        }
        m_p += n;
        return true;
    }

    // A member key and its colon, the value still to come.
    [[nodiscard]] bool key() noexcept
    {
        ws();
        if (!eat('"') || !string())
        {
            return false;
        }
        ws();
        return eat(':');
    }

    [[nodiscard]] bool document()
    {
        bool want_value = true;
        for (;;)
        {
            if (want_value)
            {
                ws();
                const u32 depth = static_cast<u32>(m_stack.size());
                if (depth > m_result.max_depth)
                {
                    m_result.max_depth = depth;
                }
                if (m_p >= m_end)
                {
                    return false;
                }
                const u8 c = *m_p;
                if (c == '{' || c == '[')
                {
                    ++m_p;
                    m_stack.push_back(c);
                    ws();
                    if (eat(c == '{' ? u8{'}'} : u8{']'}))
                    {
                        m_stack.pop_back();
                        want_value = false;
                        continue;
                    }
                    if (c == '{' && !key())
                    {
                        return false;
                    }
                    continue;
                }
                bool ok = false;
                if (c == '"')
                {
                    ++m_p;
                    ok = string();
                }
                else if (c == 't')
                {
                    ok = literal("true");
                }
                else if (c == 'f')
                {
                    ok = literal("false");
                }
                else if (c == 'n')
                {
                    ok = literal("null");
                }
                else
                {
                    ok = number();
                }
                if (!ok)
                {
                    return false;
                }
                want_value = false;
                continue;
            }
            if (m_stack.empty())
            {
                ws();
                return m_p == m_end;
            }
            ws();
            const u8 open = m_stack.back();
            if (eat(','))
            {
                if (open == '{' && !key())
                {
                    return false;
                }
                want_value = true;
                continue;
            }
            if (eat(open == '{' ? u8{'}'} : u8{']'}))
            {
                m_stack.pop_back();
                continue;
            }
            return false;
        }
    }

    const u8*  m_p;
    const u8*  m_end;
    Array<u8>  m_stack;
    Validation m_result;
};

[[nodiscard]] Validation validate(StringView text, crd::memory::IAllocator* alloc)
{
    const auto* p = reinterpret_cast<const u8*>(text.data());
    return Validator(p, text.size(), alloc).run();
}

// The element spans of a JSON array's inner text (between its brackets), split at its top-level commas.
void split_elements(StringView inner, Array<StringView>& out)
{
    usize start     = 0U;
    u32   depth     = 0U;
    bool  in_string = false;
    for (usize i = 0U; i < inner.size(); ++i)
    {
        const char c = inner[i];
        if (in_string)
        {
            if (c == '\\')
            {
                ++i;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
        }
        else if (c == '}' || c == ']')
        {
            --depth;
        }
        else if (c == ',' && depth == 0U)
        {
            out.push_back(inner.substr(start, i - start));
            start = i + 1U;
        }
    }
    if (!inner.empty())
    {
        out.push_back(inner.substr(start));
    }
}

// The inner text of the array value of the root object's member `key` (the document is valid JSON).
[[nodiscard]] bool top_level_array(StringView doc, StringView key, StringView& inner)
{
    u32  depth     = 0U;
    bool in_string = false;
    for (usize i = 0U; i < doc.size(); ++i)
    {
        const char c = doc[i];
        if (in_string)
        {
            if (c == '\\')
            {
                ++i;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
        {
            const bool member = depth == 1U && i + key.size() + 4U <= doc.size() &&
                                doc.substr(i + 1U, key.size()) == key &&
                                doc.substr(i + 1U + key.size(), 3U) == StringView{"\":["};
            if (member)
            {
                const usize begin = i + key.size() + 4U;
                u32         level = 1U;
                bool        quote = false;
                for (usize k = begin; k < doc.size(); ++k)
                {
                    const char d = doc[k];
                    if (quote)
                    {
                        if (d == '\\')
                        {
                            ++k;
                        }
                        else if (d == '"')
                        {
                            quote = false;
                        }
                        continue;
                    }
                    if (d == '"')
                    {
                        quote = true;
                    }
                    else if (d == '{' || d == '[')
                    {
                        ++level;
                    }
                    else if ((d == '}' || d == ']') && --level == 0U)
                    {
                        inner = doc.substr(begin, k - begin);
                        return true;
                    }
                }
                return false;
            }
            in_string = true;
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
        }
        else if (c == '}' || c == ']')
        {
            --depth;
        }
    }
    return false;
}

// ---- the documented service contract, restated ----------------------------------------------------------------------

constexpr const char* kStatusNames[] = {"ok",           "unsupported-schema", "unknown-command", "unauthorized",
                                        "oversized",    "bad-argument",       "stale-cursor",    "cancelled",
                                        "unavailable",  "failed"};
constexpr DiagStatus kStatuses[] = {DiagStatus::Ok,          DiagStatus::UnsupportedSchema, DiagStatus::UnknownCommand,
                                    DiagStatus::Unauthorized, DiagStatus::Oversized,        DiagStatus::BadArgument,
                                    DiagStatus::StaleCursor,  DiagStatus::Cancelled,        DiagStatus::Unavailable,
                                    DiagStatus::Failed};
constexpr const char* kAuthorityNames[] = {"read",   "record",         "inject", "remote-enable",
                                           "upload", "process-memory", "execute"};

void grant_text(DiagAuthoritySet grant, String& out)
{
    for (u32 i = 0U; i < 7U; ++i)
    {
        if ((grant & (1U << i)) != 0U)
        {
            if (!out.empty())
            {
                out.push_back(',');
            }
            out.append(StringView{kAuthorityNames[i]});
        }
    }
    if (out.empty())
    {
        out.append(StringView{"none"});
    }
}

[[nodiscard]] bool granted(DiagAuthoritySet grant, DiagAuthority a) noexcept
{
    const auto bit = static_cast<DiagAuthoritySet>(a);
    return a != DiagAuthority::None && (grant & bit) == bit;
}

[[nodiscard]] bool safe_component_char(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

// A relative path of plain components, none empty, "." or "..".
[[nodiscard]] bool safe_path(StringView path) noexcept
{
    if (path.empty())
    {
        return false;
    }
    usize start = 0U;
    for (usize i = 0U; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            const StringView part = path.substr(start, i - start);
            if (part.empty() || part == StringView{"."} || part == StringView{".."})
            {
                return false;
            }
            start = i + 1U;
            continue;
        }
        if (!safe_component_char(path[i]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool arg_name_ok(StringView name) noexcept
{
    if (name.empty() || name.size() > perf::kDiagMaxArgNameBytes)
    {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                               });
}

struct Spec
{
    StringView    name;
    DiagAuthority authority  = DiagAuthority::Read;
    DiagAuthority also       = DiagAuthority::None;
    bool          takes_path = false;
    bool          takes_args = false;
};

// The commands registered with an argument check, as their modules declare them.
constexpr const char* kCheckedCommands[] = {"fault.arm", "flight.trigger", "program.inspect", "replay.record",
                                            "replay.run"};

enum class Stage : u8
{
    Refused, // a status the request must be refused with, before any work
    Page,    // a page cut from the retained snapshot
    Handler, // the handler runs (a new snapshot)
};

struct Expect
{
    Stage      stage  = Stage::Refused;
    DiagStatus status = DiagStatus::Failed;
    u32        index  = kNone;
};

struct Model
{
    explicit Model(crd::memory::IAllocator* alloc) : specs(alloc) {}

    Array<Spec>      specs;
    DiagAuthoritySet grant      = 0U;
    bool             root       = false;
    u64              generation = 0U;    // the service's snapshot generation
    u32              of         = kNone; // the retained snapshot's command
    u64              items      = 0U;    // the retained snapshot's items
};

[[nodiscard]] Expect refused(DiagStatus s) noexcept
{
    Expect e;
    e.status = s;
    return e;
}

// Every check before the command's own argument check, in the documented order.
[[nodiscard]] Expect predict_request(const Model& m, const DiagRequest& r)
{
    if (r.schema_version != perf::kDiagCommandSchemaVersion)
    {
        return refused(DiagStatus::UnsupportedSchema);
    }
    u32 index = kNone;
    for (usize i = 0U; i < m.specs.size(); ++i)
    {
        if (m.specs[i].name == r.command)
        {
            index = static_cast<u32>(i);
        }
    }
    if (index == kNone)
    {
        return refused(DiagStatus::UnknownCommand);
    }
    const Spec& spec = m.specs[index];
    if (!granted(m.grant, spec.authority) || (spec.also != DiagAuthority::None && !granted(m.grant, spec.also)))
    {
        return refused(DiagStatus::Unauthorized);
    }
    if (r.page_items > perf::kDiagMaxPageItems || r.page_bytes > perf::kDiagMaxPageBytes ||
        r.path.size() > perf::kDiagMaxPathBytes || r.args.size() > perf::kDiagMaxArgs)
    {
        return refused(DiagStatus::Oversized);
    }
    for (const DiagArg& a : r.args)
    {
        if (a.name.size() > perf::kDiagMaxArgNameBytes || a.value.size() > perf::kDiagMaxArgValueBytes)
        {
            return refused(DiagStatus::Oversized);
        }
    }
    if (r.page_bytes != 0U && r.page_bytes < perf::kDiagMinPageBytes)
    {
        return refused(DiagStatus::BadArgument);
    }
    if (!spec.takes_path && !r.path.empty())
    {
        return refused(DiagStatus::BadArgument);
    }
    if (spec.takes_path)
    {
        if (!safe_path(r.path))
        {
            return refused(DiagStatus::BadArgument);
        }
        if (!m.root)
        {
            return refused(DiagStatus::Unavailable);
        }
    }
    if (!r.args.empty())
    {
        if (!spec.takes_args)
        {
            return refused(DiagStatus::BadArgument);
        }
        for (usize i = 0U; i < r.args.size(); ++i)
        {
            if (!arg_name_ok(r.args[i].name))
            {
                return refused(DiagStatus::BadArgument);
            }
            for (usize k = 0U; k < i; ++k)
            {
                if (r.args[k].name == r.args[i].name)
                {
                    return refused(DiagStatus::BadArgument);
                }
            }
        }
    }
    Expect e;
    e.stage = Stage::Handler; // refined by predict_cursor
    e.index = index;
    return e;
}

// The checks after the command's argument check.
[[nodiscard]] Expect predict_cursor(const Model& m, const DiagRequest& r, u32 index)
{
    if (r.cursor == 0U)
    {
        Expect e;
        e.stage = Stage::Handler;
        e.index = index;
        return e;
    }
    const u64 generation = r.cursor >> perf::kDiagCursorOffsetBits;
    const u64 offset     = r.cursor & ((1ULL << perf::kDiagCursorOffsetBits) - 1ULL);
    if (generation == 0U || generation != m.generation || m.of != index)
    {
        return refused(DiagStatus::StaleCursor);
    }
    if (offset > m.items)
    {
        return refused(DiagStatus::BadArgument);
    }
    Expect e;
    e.stage = Stage::Page;
    e.index = index;
    return e;
}

// ---- a service document, read back ---------------------------------------------------------------------------------

struct Document
{
    DiagStatus status       = DiagStatus::Failed;
    u64        generation   = 0U;
    u64        cursor       = 0U;
    u64        next_cursor  = 0U;
    u64        total        = 0U;
    u64        items        = 0U;
    u64        dropped      = 0U;
    bool       complete     = false;
    bool       byte_bounded = false;
    usize      item_bytes   = 0U; // the page's serialized items with their separating commas
    usize      first_item   = 0U; // the first item's bytes (0 for an empty page)
};

[[nodiscard]] u64 number_of(const json::JsonDoc& doc, const char* key)
{
    const u32 node = json::find(doc, doc.root, key);
    CRD_FUZZ_REQUIRE(node != json::kInvalid && doc.nodes[node].type == json::JsonType::Number);
    const double v = doc.nodes[node].number;
    CRD_FUZZ_REQUIRE(v >= 0.0 && v <= kMaxExact && v == std::floor(v));
    return static_cast<u64>(v);
}

[[nodiscard]] bool flag_of(const json::JsonDoc& doc, const char* key)
{
    const u32 node = json::find(doc, doc.root, key);
    CRD_FUZZ_REQUIRE(node != json::kInvalid && doc.nodes[node].type == json::JsonType::Bool);
    return doc.nodes[node].boolean;
}

[[nodiscard]] StringView string_of(const json::JsonDoc& doc, u32 parent, const char* key)
{
    const u32 node = json::find(doc, parent, key);
    CRD_FUZZ_REQUIRE(node != json::kInvalid && doc.nodes[node].type == json::JsonType::String);
    return StringView{doc.strings.data() + doc.nodes[node].str_off, doc.nodes[node].str_len};
}

// The command echo the document must carry: clipped at kDiagMaxCommandBytes, never inside a sequence, sanitized.
void expected_command(StringView command, String& out)
{
    StringView kept = command;
    if (kept.size() > perf::kDiagMaxCommandBytes)
    {
        usize cut = perf::kDiagMaxCommandBytes;
        while (cut > 0U && (static_cast<u8>(command[cut]) & 0xC0U) == 0x80U)
        {
            --cut;
        }
        kept = command.substr(0U, cut);
    }
    sanitized(kept, out);
}

// Read a service document for `request` and check everything it promises on its own.
[[nodiscard]] Document read_document(StringView text, const DiagRequest& request, const Model& m,
                                     crd::memory::IAllocator* alloc)
{
    const Validation v = validate(text, alloc);
    CRD_FUZZ_REQUIRE(v.valid && !v.raw_line_break);
    json::JsonDoc doc(alloc);
    CRD_FUZZ_REQUIRE(json::parse(bytes_of(text), doc));
    CRD_FUZZ_REQUIRE(doc.nodes[doc.root].type == json::JsonType::Object);

    Document d;
    CRD_FUZZ_REQUIRE(number_of(doc, "schema") == perf::kDiagCommandSchemaVersion);
    String command(alloc);
    expected_command(request.command, command);
    CRD_FUZZ_REQUIRE(string_of(doc, doc.root, "command") == view(command));
    const StringView status = string_of(doc, doc.root, "status");
    bool             named  = false;
    for (usize i = 0U; i < 10U; ++i)
    {
        if (status == StringView{kStatusNames[i]})
        {
            d.status = kStatuses[i];
            named    = true;
        }
    }
    CRD_FUZZ_REQUIRE(named);
    const bool ok = d.status == DiagStatus::Ok;
    CRD_FUZZ_REQUIRE(flag_of(doc, "ok") == ok);
    CRD_FUZZ_REQUIRE(string_of(doc, doc.root, "reason").empty() == ok);
    String grant(alloc);
    grant_text(m.grant, grant);
    CRD_FUZZ_REQUIRE(string_of(doc, doc.root, "grant") == view(grant));
    d.generation = number_of(doc, "generation");
    // A refusal echoes the request's cursor, which a native caller can set past 2^53 (only the cursors the service
    // issues are promised exact in a double): past it, the echo is compared as a double.
    const u32 cursor = json::find(doc, doc.root, "cursor");
    CRD_FUZZ_REQUIRE(cursor != json::kInvalid && doc.nodes[cursor].type == json::JsonType::Number);
    const double cursor_value = doc.nodes[cursor].number;
    if (cursor_value > kMaxExact)
    {
        CRD_FUZZ_REQUIRE(cursor_value == static_cast<double>(request.cursor));
        d.cursor = request.cursor;
    }
    else
    {
        d.cursor = number_of(doc, "cursor");
    }
    d.next_cursor  = number_of(doc, "next_cursor");
    d.total        = number_of(doc, "total");
    d.items        = number_of(doc, "page_items");
    d.dropped      = number_of(doc, "dropped");
    d.complete     = flag_of(doc, "complete");
    d.byte_bounded = flag_of(doc, "byte_bounded");
    const u32 summary = json::find(doc, doc.root, "summary");
    CRD_FUZZ_REQUIRE(summary != json::kInvalid && doc.nodes[summary].type == json::JsonType::Object);
    const u32 items = json::find(doc, doc.root, "items");
    CRD_FUZZ_REQUIRE(items != json::kInvalid && doc.nodes[items].type == json::JsonType::Array);
    CRD_FUZZ_REQUIRE(doc.nodes[items].count == d.items);

    StringView inner;
    CRD_FUZZ_REQUIRE(top_level_array(text, StringView{"items"}, inner));
    Array<StringView> elements(alloc);
    split_elements(inner, elements);
    CRD_FUZZ_REQUIRE(elements.size() == d.items);
    d.item_bytes = inner.size();
    for (const StringView e : elements)
    {
        CRD_FUZZ_REQUIRE(!e.empty() && e[0] == '{' && e.size() <= perf::kDiagMaxItemBytes);
    }
    d.first_item = elements.empty() ? 0U : elements[0].size();
    if (!ok)
    {
        CRD_FUZZ_REQUIRE(d.generation == 0U && d.next_cursor == 0U && d.total == 0U && d.items == 0U &&
                         d.dropped == 0U && !d.complete && !d.byte_bounded && d.cursor == request.cursor);
        CRD_FUZZ_REQUIRE(doc.nodes[summary].count == 0U);
        return d;
    }
    const u32 max_items = request.page_items == 0U ? perf::kDiagDefaultPageItems : request.page_items;
    const u32 max_bytes = request.page_bytes == 0U ? perf::kDiagDefaultPageBytes : request.page_bytes;
    const u64 base      = d.generation << perf::kDiagCursorOffsetBits;
    const u64 offset    = d.cursor & ((1ULL << perf::kDiagCursorOffsetBits) - 1ULL);
    CRD_FUZZ_REQUIRE(d.generation != 0U && (d.cursor >> perf::kDiagCursorOffsetBits) == d.generation);
    CRD_FUZZ_REQUIRE(d.items <= max_items && d.item_bytes <= max_bytes && offset + d.items <= d.total);
    CRD_FUZZ_REQUIRE(d.complete == (offset + d.items == d.total));
    CRD_FUZZ_REQUIRE(d.next_cursor == (d.complete ? 0U : (base | (offset + d.items))));
    CRD_FUZZ_REQUIRE(d.complete || d.items > 0U); // every page before the end makes progress
    // A page that stops before its item bound and before the end stopped on its byte bound, and says so.
    CRD_FUZZ_REQUIRE(d.byte_bounded == (!d.complete && d.items < max_items));
    return d;
}

// ---- the run: one host, one service ---------------------------------------------------------------------------------

struct Host
{
    DiagAuthoritySet grant = static_cast<DiagAuthoritySet>(DiagAuthority::Read);
    bool             root  = false;
};

// The optional "#host <grant>[ root]" line; returns the bytes it took (with its line break).
[[nodiscard]] usize parse_host(StringView input, Host& host)
{
    if (input.substr(0U, 6U) != StringView{"#host "})
    {
        return 0U;
    }
    usize end = input.find('\n');
    if (end == StringView::npos)
    {
        end = input.size();
    }
    StringView rest  = input.substr(6U, end - 6U);
    const usize space = rest.find(' ');
    const StringView grant = space == StringView::npos ? rest : rest.substr(0U, space);
    DiagAuthoritySet set   = 0U;
    if (perf::parse_authority_list(grant, set))
    {
        host.grant = set;
    }
    host.root = space != StringView::npos && rest.substr(space + 1U) == StringView{"root"};
    return end < input.size() ? end + 1U : end;
}

// Digits only, saturating; an empty field keeps `fallback`.
[[nodiscard]] u64 field_number(StringView field, u64 fallback, u64 limit) noexcept
{
    if (field.empty())
    {
        return fallback;
    }
    u64 v = 0U;
    for (const char c : field)
    {
        if (c >= '0' && c <= '9')
        {
            const u64 d = static_cast<u64>(c - '0');
            v           = v > (limit - d) / 10U ? limit : v * 10U + d;
        }
    }
    return v;
}

class Run
{
public:
    Run(const Host& host, crd::memory::IAllocator* alloc)
        : m_alloc(alloc),
          m_model(alloc),
          m_host_sink(m_recorder),
          m_service(host.grant, config(host), alloc)
    {
        perf::FlightPolicy policy;
        policy.capacity    = 16U;
        policy.pre_events  = 8U;
        policy.post_events = 8U;
        CRD_FUZZ_REQUIRE(m_recorder.init(policy) == perf::FlightPolicyStatus::Committed);
        m_host_sink.install_sink();
        CRD_FUZZ_REQUIRE(crd::ceridc::bind_diag_commands(m_service));
        CRD_FUZZ_REQUIRE(perf::register_flight_commands(m_service, m_host_sink));
        CRD_FUZZ_REQUIRE(perf::register_fault_commands(m_service, m_injector));

        m_model.grant = m_service.grant();
        m_model.root  = host.root;
        for (u32 i = 0U; i < m_service.command_count(); ++i)
        {
            const perf::DiagCommandSpec* spec = m_service.command_at(i);
            Spec                         s;
            s.name       = spec->name;
            s.authority  = spec->authority;
            s.also       = spec->also;
            s.takes_path = spec->takes_path;
            for (const char* checked : kCheckedCommands)
            {
                s.takes_args = s.takes_args || spec->name == StringView{checked};
            }
            m_model.specs.push_back(s);
        }
    }

    void line(StringView text)
    {
        if (text.empty())
        {
            return;
        }
        if (text[0] == '!')
        {
            native(text.substr(1U));
            return;
        }
        if (text[0] == '@')
        {
            String spliced(m_alloc);
            spliced.append(StringView{R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"diag",)"
                                      R"("arguments":)"});
            spliced.append(text.substr(1U));
            spliced.append(StringView{"}}"});
            transport(view(spliced));
            return;
        }
        transport(text);
    }

private:
    // The recorder and injector are members declared before the service, so they exist when it is built.
    [[nodiscard]] perf::DiagServiceConfig config(const Host& host) const
    {
        perf::DiagServiceConfig c;
        c.root            = host.root ? kAbsentRoot : StringView{};
        c.flight_recorder = &m_recorder;
        c.fault_injector  = &m_injector;
        return c;
    }

    // ---- native requests --------------------------------------------------------------------------------------------

    void native(StringView fields)
    {
        Array<StringView> parts(m_alloc);
        usize             start = 0U;
        for (usize i = 0U; i <= fields.size(); ++i)
        {
            if (i == fields.size() || fields[i] == '|')
            {
                parts.push_back(fields.substr(start, i - start));
                start = i + 1U;
            }
        }
        const auto part = [&parts](usize i)
        {
            return i < parts.size() ? parts[i] : StringView{};
        };
        Array<DiagArg> args(m_alloc);
        for (usize i = 6U; i < parts.size() && args.size() < kMaxNativeArgs; ++i)
        {
            const usize eq = parts[i].find('=');
            DiagArg     a;
            a.name  = eq == StringView::npos ? parts[i] : parts[i].substr(0U, eq);
            a.value = eq == StringView::npos ? StringView{} : parts[i].substr(eq + 1U);
            args.push_back(a);
        }
        DiagRequest r;
        r.command        = part(0U);
        r.path           = part(1U);
        r.cursor         = field_number(part(2U), 0U, ~u64{0U});
        r.page_items     = static_cast<u32>(field_number(part(3U), 0U, 0xFFFFFFFFULL));
        r.page_bytes     = static_cast<u32>(field_number(part(4U), 0U, 0xFFFFFFFFULL));
        r.schema_version = static_cast<u32>(field_number(part(5U), perf::kDiagCommandSchemaVersion, 0xFFFFFFFFULL));
        r.args           = ConstSpan<DiagArg>(args.data(), args.size());

        const u64        before = m_service.handler_runs();
        const DiagResult result = m_service.execute(r);
        const u64        delta  = m_service.handler_runs() - before;
        const Document   d      = read_document(view(result.json), r, m_model, m_alloc);
        // The typed result is the document.
        CRD_FUZZ_REQUIRE(result.status == d.status && result.generation == d.generation && result.cursor == d.cursor &&
                         result.next_cursor == d.next_cursor && result.total == d.total && result.items == d.items &&
                         result.dropped == d.dropped && result.complete == d.complete &&
                         result.byte_bounded == d.byte_bounded);
        CRD_FUZZ_REQUIRE(result.reason.empty() == (result.status == DiagStatus::Ok));
        String reason(m_alloc);
        sanitized(view(result.reason), reason);
        json::JsonDoc doc(m_alloc);
        CRD_FUZZ_REQUIRE(json::parse(bytes_of(view(result.json)), doc));
        CRD_FUZZ_REQUIRE(string_of(doc, doc.root, "reason") == view(reason));
        against_model(r, d, delta);
        walk(r, d);
    }

    // The service's answer against the documented order; updates the model's view of the retained snapshot.
    void against_model(const DiagRequest& r, const Document& d, u64 delta)
    {
        CRD_FUZZ_REQUIRE(d.status != DiagStatus::Cancelled); // nothing cancels
        const Expect pre = predict_request(m_model, r);
        if (pre.stage == Stage::Refused)
        {
            CRD_FUZZ_REQUIRE(d.status == pre.status && delta == 0U);
            return;
        }
        const Spec& spec = m_model.specs[pre.index];
        // A command's own argument check answers only bad-argument or unavailable, before any work.
        const bool check_refused =
            spec.takes_args && delta == 0U && (d.status == DiagStatus::BadArgument || d.status == DiagStatus::Unavailable);
        const Expect post = predict_cursor(m_model, r, pre.index);
        if (post.stage == Stage::Refused)
        {
            CRD_FUZZ_REQUIRE((d.status == post.status && delta == 0U) || check_refused);
            return;
        }
        if (post.stage == Stage::Page)
        {
            const bool page = d.status == DiagStatus::Ok && delta == 0U && d.generation == m_model.generation &&
                              d.total == m_model.items && d.cursor == r.cursor;
            CRD_FUZZ_REQUIRE(page || check_refused);
            return;
        }
        if (check_refused)
        {
            return;
        }
        CRD_FUZZ_REQUIRE(delta == 1U);
        if (d.status == DiagStatus::Ok)
        {
            CRD_FUZZ_REQUIRE(d.generation == m_model.generation + 1U && d.cursor == (d.generation << 20U));
            m_model.generation = d.generation;
            m_model.of         = pre.index;
            m_model.items      = d.total;
        }
        else
        {
            m_model.of = kNone; // a failed handler drops the retained snapshot
        }
    }

    // Page an Ok answer's snapshot to its end: contiguous, progressing, byte bounds honoured, each page repeatable.
    void walk(const DiagRequest& r, const Document& first)
    {
        if (first.status != DiagStatus::Ok)
        {
            return;
        }
        const u32 max_bytes = r.page_bytes == 0U ? perf::kDiagDefaultPageBytes : r.page_bytes;
        Document  previous  = first;
        u64       covered   = (first.cursor & ((1ULL << perf::kDiagCursorOffsetBits) - 1ULL)) + first.items;
        for (u32 page = 0U; page < kMaxWalkPages && previous.next_cursor != 0U; ++page)
        {
            DiagRequest next = r;
            next.cursor      = previous.next_cursor;
            const u64        before = m_service.handler_runs();
            const DiagResult again  = m_service.execute(next);
            const DiagResult repeat = m_service.execute(next);
            CRD_FUZZ_REQUIRE(m_service.handler_runs() == before);
            CRD_FUZZ_REQUIRE(again.json.size() == repeat.json.size() &&
                             std::memcmp(again.json.data(), repeat.json.data(), again.json.size()) == 0);
            const Document d = read_document(view(again.json), next, m_model, m_alloc);
            CRD_FUZZ_REQUIRE(d.status == DiagStatus::Ok && d.generation == first.generation &&
                             d.total == first.total && d.cursor == next.cursor && d.items > 0U);
            // The page before stopped on its byte bound exactly when this page's first item would not have fit.
            if (previous.byte_bounded)
            {
                const usize with = previous.item_bytes + (previous.items > 0U ? 1U : 0U) + d.first_item;
                CRD_FUZZ_REQUIRE(with > max_bytes);
            }
            covered += d.items;
            previous = d;
        }
        CRD_FUZZ_REQUIRE(previous.next_cursor != 0U || covered == first.total);
    }

    // ---- the MCP transport ------------------------------------------------------------------------------------------

    // Whether the line could reach one of ceridc's file verbs, by the decoded names or, conservatively, by their bytes
    // before a NUL (cut to 63, the transport's old fixed buffer): such a line is never fed.
    [[nodiscard]] static bool could_name(const json::JsonDoc& doc, u32 node, StringView name)
    {
        if (node == json::kInvalid || doc.nodes[node].type != json::JsonType::String)
        {
            return false;
        }
        StringView s{doc.strings.data() + doc.nodes[node].str_off, doc.nodes[node].str_len};
        if (s == name)
        {
            return true;
        }
        const usize nul = s.find('\0');
        s               = s.substr(0U, nul == StringView::npos ? s.size() : nul);
        return s.substr(0U, 63U) == name;
    }

    [[nodiscard]] static bool reaches_file_verb(const json::JsonDoc& doc)
    {
        if (doc.nodes[doc.root].type != json::JsonType::Object ||
            !could_name(doc, json::find(doc, doc.root, "method"), StringView{"tools/call"}))
        {
            return false;
        }
        const u32             name    = json::find(doc, json::find(doc, doc.root, "params"), "name");
        constexpr const char* verbs[] = {"import", "cook", "query", "instantiate", "sequence", "render", "export"};
        return std::ranges::any_of(verbs,
                                   [&doc, name](const char* verb)
                                   {
                                       return could_name(doc, name, StringView{verb});
                                   });
    }

    // The diag tool's documented decoding of `arguments`. False: the call is a protocol error (-32602).
    [[nodiscard]] static bool decode_diag(const json::JsonDoc& doc, u32 args, DiagRequest& r, Array<DiagArg>& named)
    {
        if (args == json::kInvalid || doc.nodes[args].type != json::JsonType::Object)
        {
            return false;
        }
        const auto text = [&doc](u32 node)
        {
            return StringView{doc.strings.data() + doc.nodes[node].str_off, doc.nodes[node].str_len};
        };
        const u32 command = json::find(doc, args, "command");
        if (command == json::kInvalid || doc.nodes[command].type != json::JsonType::String)
        {
            return false;
        }
        r.command      = text(command);
        const u32 path = json::find(doc, args, "path");
        if (path != json::kInvalid)
        {
            if (doc.nodes[path].type != json::JsonType::String)
            {
                return false;
            }
            r.path = text(path);
        }
        double values[4] = {0.0, 0.0, 0.0, static_cast<double>(perf::kDiagCommandSchemaVersion)};
        const char* const keys[4] = {"cursor", "page_items", "page_bytes", "schema"};
        for (usize i = 0U; i < 4U; ++i)
        {
            const u32 node = json::find(doc, args, keys[i]);
            if (node == json::kInvalid)
            {
                continue;
            }
            if (doc.nodes[node].type != json::JsonType::Number)
            {
                return false;
            }
            const double v = doc.nodes[node].number;
            if (!(v >= 0.0 && v <= kMaxExact) || v != std::floor(v))
            {
                return false;
            }
            values[i] = v;
        }
        const u32 members = json::find(doc, args, "args");
        if (members != json::kInvalid)
        {
            if (doc.nodes[members].type != json::JsonType::Object)
            {
                return false;
            }
            for (u32 m = doc.nodes[members].child; m != json::kInvalid; m = doc.nodes[m].next)
            {
                if (doc.nodes[m].type != json::JsonType::String)
                {
                    return false;
                }
                DiagArg a;
                a.name  = StringView{doc.strings.data() + doc.nodes[m].key_off, doc.nodes[m].key_len};
                a.value = text(m);
                named.push_back(a);
            }
        }
        const auto clamp = [](double v)
        {
            return v < 4294967295.0 ? static_cast<u32>(v) : 0xFFFFFFFFU;
        };
        r.cursor         = static_cast<u64>(values[0]);
        r.page_items     = clamp(values[1]);
        r.page_bytes     = clamp(values[2]);
        r.schema_version = clamp(values[3]);
        r.args           = ConstSpan<DiagArg>(named.data(), named.size());
        return true;
    }

    struct Reply
    {
        bool     error  = false;
        crd::i64 code   = 0;
        u32      result = json::kInvalid;
    };

    // Read a reply: one line of valid JSON, "jsonrpc":"2.0", and the expected id (`id` kInvalid: null).
    [[nodiscard]] Reply read_reply(StringView reply, json::JsonDoc& out, const json::JsonDoc& request, u32 id)
    {
        const Validation v = validate(reply, m_alloc);
        CRD_FUZZ_REQUIRE(v.valid && !v.raw_line_break);
        CRD_FUZZ_REQUIRE(json::parse(bytes_of(reply), out));
        CRD_FUZZ_REQUIRE(out.nodes[out.root].type == json::JsonType::Object);
        CRD_FUZZ_REQUIRE(string_of(out, out.root, "jsonrpc") == StringView{"2.0"});
        const u32 echoed = json::find(out, out.root, "id");
        CRD_FUZZ_REQUIRE(echoed != json::kInvalid);
        const json::JsonNode& e = out.nodes[echoed];
        if (id == json::kInvalid)
        {
            CRD_FUZZ_REQUIRE(e.type == json::JsonType::Null);
        }
        else
        {
            const json::JsonNode& want = request.nodes[id];
            CRD_FUZZ_REQUIRE(e.type == want.type);
            if (want.type == json::JsonType::String)
            {
                const StringView a{out.strings.data() + e.str_off, e.str_len};
                const StringView b{request.strings.data() + want.str_off, want.str_len};
                CRD_FUZZ_REQUIRE(a == b);
            }
            else
            {
                CRD_FUZZ_REQUIRE(e.number == want.number);
            }
        }
        Reply r;
        const u32 error = json::find(out, out.root, "error");
        if (error != json::kInvalid)
        {
            r.error         = true;
            const u32 code  = json::find(out, error, "code");
            CRD_FUZZ_REQUIRE(code != json::kInvalid && out.nodes[code].type == json::JsonType::Number);
            r.code = static_cast<crd::i64>(out.nodes[code].number);
            CRD_FUZZ_REQUIRE(json::find(out, out.root, "result") == json::kInvalid);
            return r;
        }
        r.result = json::find(out, out.root, "result");
        CRD_FUZZ_REQUIRE(r.result != json::kInvalid && out.nodes[r.result].type == json::JsonType::Object);
        return r;
    }

    void expect_error(StringView reply, const json::JsonDoc& request, u32 id, crd::i64 code)
    {
        json::JsonDoc out(m_alloc);
        const Reply   r = read_reply(reply, out, request, id);
        CRD_FUZZ_REQUIRE(r.error && r.code == code);
    }

    void transport(StringView line)
    {
        json::JsonDoc    doc(m_alloc);
        const bool       parsed = json::parse(bytes_of(line), doc);
        const Validation v      = validate(line, m_alloc);
        CRD_FUZZ_REQUIRE(parsed == (v.valid && v.max_depth <= kMaxDepth && v.numbers_in_range));
        if (parsed && reaches_file_verb(doc))
        {
            return;
        }
        const u64    before = m_service.handler_runs();
        const String reply  = crd::ceridc::mcp_handle(bytes_of(line), m_alloc, &m_service);
        const u64    delta  = m_service.handler_runs() - before;
        const StringView text = view(reply);
        if (!parsed)
        {
            expect_error(text, doc, json::kInvalid, -32700);
            CRD_FUZZ_REQUIRE(delta == 0U);
            return;
        }
        if (doc.nodes[doc.root].type != json::JsonType::Object)
        {
            expect_error(text, doc, json::kInvalid, -32600);
            CRD_FUZZ_REQUIRE(delta == 0U);
            return;
        }
        const u32 id = json::find(doc, doc.root, "id");
        if (id == json::kInvalid)
        {
            CRD_FUZZ_REQUIRE(text.empty() && delta == 0U); // a notification is not answered and runs nothing
            return;
        }
        const json::JsonNode& idn = doc.nodes[id];
        const bool echoable = idn.type == json::JsonType::String ||
                              (idn.type == json::JsonType::Number && idn.number >= -kMaxExact &&
                               idn.number <= kMaxExact && idn.number == std::floor(idn.number));
        if (!echoable)
        {
            expect_error(text, doc, json::kInvalid, -32600);
            CRD_FUZZ_REQUIRE(delta == 0U);
            return;
        }
        const u32 method = json::find(doc, doc.root, "method");
        if (method == json::kInvalid || doc.nodes[method].type != json::JsonType::String)
        {
            expect_error(text, doc, id, -32600);
            CRD_FUZZ_REQUIRE(delta == 0U);
            return;
        }
        if (json::str_value_eq(doc, method, "initialize") || json::str_value_eq(doc, method, "ping") ||
            json::str_value_eq(doc, method, "tools/list"))
        {
            json::JsonDoc out(m_alloc);
            const Reply   r = read_reply(text, out, doc, id);
            CRD_FUZZ_REQUIRE(!r.error && delta == 0U);
            if (json::str_value_eq(doc, method, "tools/list"))
            {
                const u32 tools = json::find(out, r.result, "tools");
                CRD_FUZZ_REQUIRE(tools != json::kInvalid && out.nodes[tools].type == json::JsonType::Array);
                bool diag = false;
                for (u32 t = out.nodes[tools].child; t != json::kInvalid; t = out.nodes[t].next)
                {
                    diag = diag || json::str_value_eq(out, json::find(out, t, "name"), "diag");
                }
                CRD_FUZZ_REQUIRE(diag);
            }
            return;
        }
        if (!json::str_value_eq(doc, method, "tools/call"))
        {
            expect_error(text, doc, id, -32601);
            CRD_FUZZ_REQUIRE(delta == 0U);
            return;
        }
        const u32 params = json::find(doc, doc.root, "params");
        DiagRequest    request;
        Array<DiagArg> named(m_alloc);
        if (!json::str_value_eq(doc, json::find(doc, params, "name"), "diag") ||
            !decode_diag(doc, json::find(doc, params, "arguments"), request, named))
        {
            expect_error(text, doc, id, -32602);
            CRD_FUZZ_REQUIRE(delta == 0U);
            return;
        }
        json::JsonDoc out(m_alloc);
        const Reply   r = read_reply(text, out, doc, id);
        CRD_FUZZ_REQUIRE(!r.error);
        const u32 content = json::find(out, r.result, "content");
        CRD_FUZZ_REQUIRE(content != json::kInvalid && out.nodes[content].type == json::JsonType::Array &&
                         out.nodes[content].count == 1U);
        const u32 item = out.nodes[content].child;
        CRD_FUZZ_REQUIRE(string_of(out, item, "type") == StringView{"text"});
        const StringView report = string_of(out, item, "text");
        const Document   d      = read_document(report, request, m_model, m_alloc);
        const u32        is_err = json::find(out, r.result, "isError");
        CRD_FUZZ_REQUIRE(is_err != json::kInvalid && out.nodes[is_err].type == json::JsonType::Bool &&
                         out.nodes[is_err].boolean == (d.status != DiagStatus::Ok));
        against_model(request, d, delta);

        // The same request sent natively, for the page the report names, returns the report byte for byte. A refusal
        // that ran nothing is the same refusal again; a failed handler is not run twice.
        if (d.status == DiagStatus::Ok || delta == 0U)
        {
            DiagRequest again = request;
            if (d.status == DiagStatus::Ok)
            {
                again.cursor = d.cursor;
            }
            const u64        runs   = m_service.handler_runs();
            const DiagResult native = m_service.execute(again);
            CRD_FUZZ_REQUIRE(m_service.handler_runs() == runs);
            CRD_FUZZ_REQUIRE(native.json.size() == report.size() &&
                             std::memcmp(native.json.data(), report.data(), report.size()) == 0);
        }
        walk(request, d);
    }

    crd::memory::IAllocator* m_alloc;
    Model                    m_model;
    perf::FlightRecorder     m_recorder;
    perf::FlightCommandHost  m_host_sink;
    perf::FaultInjector      m_injector;
    perf::DiagCommandService m_service;
};

void run_input(StringView input)
{
    Host        host;
    const usize skip = parse_host(input, host);
    StringView  rest = input.substr(skip);

    {
        crd::fuzz::BudgetAllocator alloc;
        {
            Run run(host, &alloc);
            for (usize n = 0U; n < kMaxLines && !rest.empty(); ++n)
            {
                usize end = rest.find('\n');
                if (end == StringView::npos)
                {
                    end = rest.size();
                }
                run.line(rest.substr(0U, end));
                rest = end < rest.size() ? rest.substr(end + 1U) : StringView{};
            }
        }
        CRD_FUZZ_REQUIRE(alloc.live_bytes() == 0U); // every byte the run allocated came back
    }
}

// The seed verdict: every JSON line of the seed (spliced for '@'; native lines aside) is one the parser accepts.
bool lines_parse(const u8* data, usize size)
{
    StringView input{reinterpret_cast<const char*>(data), size};
    Host       host;
    input = input.substr(parse_host(input, host));
    crd::fuzz::BudgetAllocator alloc;
    bool                       all = true;
    while (!input.empty())
    {
        usize end = input.find('\n');
        if (end == StringView::npos)
        {
            end = input.size();
        }
        const StringView line = input.substr(0U, end);
        input                 = end < input.size() ? input.substr(end + 1U) : StringView{};
        if (line.empty() || line[0] == '!')
        {
            continue;
        }
        String text(&alloc);
        if (line[0] == '@')
        {
            text.append(StringView{R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"diag",)"
                                   R"("arguments":)"});
            text.append(line.substr(1U));
            text.append(StringView{"}}"});
        }
        else
        {
            text.append(line);
        }
        json::JsonDoc doc(&alloc);
        all = all && json::parse(bytes_of(view(text)), doc);
    }
    return all;
}

void check_root_absent()
{
    static const bool kRootAbsent = []
    {
        std::error_code ec;
        return !std::filesystem::exists(std::filesystem::path(std::string_view{kAbsentRoot}), ec);
    }();
    if (!kRootAbsent)
    {
        std::fprintf(stderr, "fuzz: %.*s exists in the working directory; the host root must not\n",
                     static_cast<int>(kAbsentRoot.size()), kAbsentRoot.data());
        std::fflush(stderr);
        crd::fuzz::fatal_exit();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    check_root_absent();
    run_input(StringView{reinterpret_cast<const char*>(data), size});
    return 0;
}

namespace
{
// Seeds with a JSON line the parser refuses; every other seed's lines all parse.
constexpr const char* kRefusedSeeds[] = {
    "truncated-request",         "number-plus",   "number-leading-zero", "number-bare-fraction",
    "number-trailing-dot",       "number-exponent-no-digit", "number-overflow", "utf8-overlong",
    "utf8-encoded-surrogate",    "utf8-past-max", "utf8-cut-sequence",   "utf8-stray-continuation",
    "escape-lone-low-surrogate", "depth-65",      "depth-20000",
};

// Checks each seed's verdict as it is written: the parser refuses a JSON line of exactly the seeds kRefusedSeeds
// names (crd::fuzz::VerdictSink's rule, with the refused seeds listed instead of the accepted ones), so a
// regenerated corpus cannot carry a boundary seed whose verdict changed silently.
class RefusedSink final : public crd::fuzz::SeedSink
{
public:
    explicit RefusedSink(crd::fuzz::SeedSink& next) : m_next(next) {}

    void add(const char* name, const u8* data, usize size) override
    {
        bool refused = false;
        for (const char* r : kRefusedSeeds)
        {
            refused = refused || std::strcmp(r, name) == 0;
        }
        if (lines_parse(data, size) == refused)
        {
            std::fprintf(stderr, "fuzz seeds: %s was %s, but the seed list says it %s\n", name,
                         refused ? "accepted" : "refused", refused ? "is refused" : "is accepted");
            std::fflush(stderr);
            crd::fuzz::fatal_exit();
        }
        m_next.add(name, data, size);
    }

private:
    crd::fuzz::SeedSink& m_next;
};

// `prefix`, `count` copies of `open`, `middle`, `count` copies of `close`, `suffix`.
void add_nested(crd::fuzz::SeedSink& sink, const char* name, const char* prefix, const char* open, usize count,
                const char* middle, const char* close, const char* suffix)
{
    String text;
    text.append(StringView{prefix});
    for (usize i = 0U; i < count; ++i)
    {
        text.append(StringView{open});
    }
    text.append(StringView{middle});
    for (usize i = 0U; i < count; ++i)
    {
        text.append(StringView{close});
    }
    text.append(StringView{suffix});
    sink.add(name, reinterpret_cast<const u8*>(text.data()), text.size());
}
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& out)
{
    RefusedSink sink(out);

    // The JSON-RPC envelope.
    sink.add_text("lifecycle",
                  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
                  "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n"
                  "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n");
    sink.add_text("id-string", "{\"jsonrpc\":\"2.0\",\"id\":\"req-\\u00e9\\\"1\",\"method\":\"ping\"}\n");
    sink.add_text("id-negative", "{\"jsonrpc\":\"2.0\",\"id\":-9007199254740991,\"method\":\"ping\"}\n");
    sink.add_text("id-null", "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}\n");
    sink.add_text("id-fraction", "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"ping\"}\n");
    sink.add_text("id-past-exact", "{\"jsonrpc\":\"2.0\",\"id\":9007199254740993,\"method\":\"ping\"}\n");
    sink.add_text("id-huge", "{\"jsonrpc\":\"2.0\",\"id\":1e300,\"method\":\"ping\"}\n");
    sink.add_text("id-object", "{\"jsonrpc\":\"2.0\",\"id\":{\"a\":1},\"method\":\"ping\"}\n");
    sink.add_text("batch-array", "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]\n");
    sink.add_text("bare-value", "42\n");
    sink.add_text("method-not-string", "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":5}\n");
    sink.add_text("method-missing", "{\"jsonrpc\":\"2.0\",\"id\":4}\n");
    sink.add_text("method-nul-suffix", "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\\u0000x\"}\n");
    sink.add_text("method-unknown", "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/list\"}\n");
    sink.add_text("tool-unknown",
                  "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"shell\"}}\n");
    sink.add_text("tool-diag-nul-suffix",
                  "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"diag\\u0000\","
                  "\"arguments\":{\"command\":\"diag.commands\"}}}\n");
    sink.add_text("tool-params-not-object",
                  "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":[1]}\n");
    sink.add_text("truncated-request", "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\"\n");

    // The owned parser's boundaries (DIAG.10a findings: a leading '+' or zero, a bare '.', a cut number token, a
    // raw byte that is not UTF-8 were accepted; a double overflow became infinity).
    sink.add_text("number-plus", "{\"jsonrpc\":\"2.0\",\"id\":+1,\"method\":\"ping\"}\n");
    sink.add_text("number-leading-zero", "{\"jsonrpc\":\"2.0\",\"id\":01,\"method\":\"ping\"}\n");
    sink.add_text("number-bare-fraction", "{\"jsonrpc\":\"2.0\",\"id\":.5,\"method\":\"ping\"}\n");
    sink.add_text("number-trailing-dot", "{\"jsonrpc\":\"2.0\",\"id\":1.,\"method\":\"ping\"}\n");
    sink.add_text("number-exponent-no-digit", "{\"jsonrpc\":\"2.0\",\"id\":1e,\"method\":\"ping\"}\n");
    sink.add_text("number-overflow", "{\"jsonrpc\":\"2.0\",\"id\":1e400,\"method\":\"ping\"}\n");
    sink.add_text("number-long-token",
                  "{\"jsonrpc\":\"2.0\",\"id\":100000000000000000000000000000000000000000000000000000000000000000"
                  "0000000000,\"method\":\"ping\"}\n");
    sink.add_text("number-forms", "@{\"command\":\"diag.commands\",\"cursor\":-0,\"page_items\":2E1,"
                                  "\"page_bytes\":1024.0,\"schema\":0.1e1}\n");
    sink.add_text("utf8-overlong", "{\"jsonrpc\":\"2.0\",\"id\":\"\xC0\xAF\",\"method\":\"ping\"}\n");
    sink.add_text("utf8-encoded-surrogate", "{\"jsonrpc\":\"2.0\",\"id\":\"\xED\xA0\x80\",\"method\":\"ping\"}\n");
    sink.add_text("utf8-past-max", "{\"jsonrpc\":\"2.0\",\"id\":\"\xF4\x90\x80\x80\",\"method\":\"ping\"}\n");
    sink.add_text("utf8-cut-sequence", "{\"jsonrpc\":\"2.0\",\"id\":\"\xE2\x82\",\"method\":\"ping\"}\n");
    sink.add_text("utf8-stray-continuation", "{\"jsonrpc\":\"2.0\",\"id\":\"\x80\",\"method\":\"ping\"}\n");
    sink.add_text("utf8-four-byte", "{\"jsonrpc\":\"2.0\",\"id\":\"\xF0\x9F\x98\x80\xE2\x82\xAC\",\"method\":\"ping\"}\n");
    sink.add_text("escape-lone-low-surrogate", "{\"jsonrpc\":\"2.0\",\"id\":\"\\udc00\",\"method\":\"ping\"}\n");
    const char* const params = R"({"jsonrpc":"2.0","id":12,"method":"ping","params":)";
    add_nested(sink, "depth-64", params, "[", 63U, "1", "]", "}\n");
    add_nested(sink, "depth-65", params, "[", 64U, "1", "]", "}\n");
    add_nested(sink, "depth-20000", "", "[", 20000U, "", "]", "\n");

    // The diag tool's decoding.
    sink.add_text("diag-list-and-page",
                  "@{\"command\":\"diag.commands\"}\n"
                  "@{\"command\":\"diag.commands\",\"page_items\":3}\n"
                  "@{\"command\":\"diag.commands\",\"cursor\":1048579,\"page_items\":3}\n"
                  "@{\"command\":\"diag.commands\",\"cursor\":1048576,\"page_items\":256,\"page_bytes\":1024}\n");
    sink.add_text("diag-capabilities", "@{\"command\":\"diag.capabilities\"}\n");
    sink.add_text("diag-refusal-order",
                  "@{\"command\":\"diag.commands\",\"schema\":2}\n"
                  "@{\"command\":\"no.such\"}\n"
                  "@{\"command\":\"capture.start\",\"page_items\":300}\n"
                  "@{\"command\":\"diag.commands\",\"page_items\":257}\n"
                  "@{\"command\":\"diag.commands\",\"page_bytes\":262145}\n"
                  "@{\"command\":\"diag.commands\",\"page_bytes\":1023}\n"
                  "@{\"command\":\"diag.commands\",\"path\":\"x\"}\n"
                  "@{\"command\":\"bundle.inspect\"}\n"
                  "@{\"command\":\"bundle.inspect\",\"path\":\"../secret\"}\n"
                  "@{\"command\":\"bundle.inspect\",\"path\":\"a/b.cdb\"}\n"
                  "@{\"command\":\"diag.commands\",\"args\":{\"x\":\"1\"}}\n");
    sink.add_text("diag-cursors",
                  "@{\"command\":\"diag.commands\",\"cursor\":1048576}\n"
                  "@{\"command\":\"diag.commands\"}\n"
                  "@{\"command\":\"diag.capabilities\",\"cursor\":1048576}\n"
                  "@{\"command\":\"diag.commands\",\"cursor\":1049575}\n"
                  "@{\"command\":\"diag.commands\",\"cursor\":2097152}\n");
    sink.add_text("diag-malformed-fields",
                  "@{\"command\":7}\n"
                  "@{\"path\":\"x\"}\n"
                  "@{\"command\":\"diag.commands\",\"path\":1}\n"
                  "@{\"command\":\"diag.commands\",\"cursor\":1.5}\n"
                  "@{\"command\":\"diag.commands\",\"cursor\":-1}\n"
                  "@{\"command\":\"diag.commands\",\"page_items\":9007199254740992}\n"
                  "@{\"command\":\"diag.commands\",\"page_items\":4294967296}\n"
                  "@{\"command\":\"diag.commands\",\"schema\":\"1\"}\n"
                  "@{\"command\":\"diag.commands\",\"args\":[]}\n"
                  "@{\"command\":\"diag.commands\",\"args\":{\"a\":1}}\n"
                  "@[]\n"
                  "@\"diag.commands\"\n");
    sink.add_text("diag-command-utf8",
                  "@{\"command\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xC3\xA9\"}\n"
                  "@{\"command\":\"\\u0000diag.commands\"}\n"
                  "@{\"command\":\"diag.commands\\u0000\"}\n");
    sink.add_text("native-requests",
                  "!diag.commands||0|2|0|1\n"
                  "!diag.commands||1048578|2|1024|1\n"
                  "!\xFF\xFE\xC3\n"
                  "!aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xE2\x82\n"
                  "!diag.commands||0|99999999999|0|1\n"
                  "!diag.commands||0|0|0|0\n"
                  "!fault.status||||||plan=x\n");
    // libFuzzer's first finding (linux-clang-fuzz): a refused cursor request's typed cursor was 0 while its document
    // named the request's cursor.
    sink.add_text("native-stale-cursor", "!no.such\n!diag.commands||1048578|2|1024|1\n");
    // A cursor past 2^53, which only a native caller can send: refused stale, its echo past what a double holds exactly.
    sink.add_text("native-cursor-past-exact", "!diag.commands\n!diag.commands||18446744073709551615\n");
    sink.add_text("native-args",
                  "#host read,record,inject root\n"
                  "!flight.trigger||||||message=\xF0\x28\x8C\x28\n"
                  "!flight.trigger||||||Message=x\n"
                  "!flight.trigger||||||message=a|message=b\n"
                  "!fault.arm||||||a=1|b=2|c=3|d=4|e=5|f=6|g=7|h=8|i=9\n"
                  "!fault.arm||||||plan=seed=1;failed-read@0+1%1000000|asset=x\n");

    // The flight and fault commands (the sandbox's), and their argument checks.
    sink.add_text("flight-commands",
                  "#host read,record root\n"
                  "@{\"command\":\"flight.status\"}\n"
                  "@{\"command\":\"flight.trigger\",\"args\":{\"message\":\"first \\u00fc\"}}\n"
                  "@{\"command\":\"flight.trigger\"}\n"
                  "@{\"command\":\"flight.status\",\"page_items\":1}\n"
                  "@{\"command\":\"flight.trigger\",\"args\":{\"msg\":\"x\"}}\n"
                  "@{\"command\":\"flight.policy\",\"path\":\"p.flight.toml\"}\n"
                  "@{\"command\":\"flight.write\",\"path\":\"out/flight.cdb\"}\n"
                  "@{\"command\":\"capture.start\"}\n"
                  "@{\"command\":\"capture.stop\",\"path\":\"c.cprof\"}\n");
    sink.add_text("fault-commands",
                  "#host read,inject root\n"
                  "@{\"command\":\"fault.status\"}\n"
                  "@{\"command\":\"fault.arm\",\"args\":{\"plan\":\"seed=3;failed-read@0+2%500000\"}}\n"
                  "@{\"command\":\"fault.status\"}\n"
                  "@{\"command\":\"fault.arm\",\"args\":{\"plan\":\"seed=3;disk-full@0+1%1\"}}\n"
                  "@{\"command\":\"fault.arm\",\"args\":{\"asset\":\"../plan.fault.toml\"}}\n"
                  "@{\"command\":\"fault.arm\",\"args\":{\"asset\":\"plans/once.fault.toml\"}}\n"
                  "@{\"command\":\"fault.arm\",\"args\":{\"plan\":\"x\",\"asset\":\"y\"}}\n"
                  "@{\"command\":\"fault.arm\"}\n"
                  "@{\"command\":\"fault.disarm\"}\n");
    sink.add_text("fault-unauthorized",
                  "@{\"command\":\"fault.arm\",\"args\":{\"plan\":\"seed=1;failed-read@0+1%1000000\"}}\n"
                  "@{\"command\":\"flight.trigger\"}\n"
                  "@{\"command\":\"replay.run\",\"path\":\"r.crrr\"}\n");

    // The CEIR commands ceridc binds, under a root with nothing in it: each argument check, then a handler that
    // finds no file.
    sink.add_text("ceir-commands",
                  "#host read,record,execute root\n"
                  "@{\"command\":\"program.provenance\",\"path\":\"demo.ceir\"}\n"
                  "@{\"command\":\"replay.prepare\",\"path\":\"demo.ceir\"}\n"
                  "@{\"command\":\"program.inspect\",\"path\":\"demo.ceir\",\"args\":{\"entry\":\"main\","
                  "\"args\":\"1,2\",\"breaks\":\"3\",\"watches\":\"3\",\"steps\":\"into,over,out\",\"max_stops\":\"4\","
                  "\"seed\":\"18446744073709551615\",\"events\":\"1,2,3\"}}\n"
                  "@{\"command\":\"program.inspect\",\"path\":\"demo.ceir\",\"args\":{\"steps\":\"jump\"}}\n"
                  "@{\"command\":\"program.inspect\",\"path\":\"demo.ceir\",\"args\":{\"max_stops\":\"0\"}}\n"
                  "@{\"command\":\"program.inspect\",\"path\":\"demo.ceir\",\"args\":{\"seed\":\"18446744073709551616\"}}\n"
                  "@{\"command\":\"program.inspect\",\"path\":\"demo.ceir\",\"args\":{\"clock\":\"wall\","
                  "\"sim_time\":\"-5\",\"sim_step\":\"1\"}}\n"
                  "@{\"command\":\"replay.record\",\"path\":\"demo.ceir\",\"args\":{\"out\":\"r.crrr\","
                  "\"executor\":\"host\",\"jobs\":\"2\"}}\n"
                  "@{\"command\":\"replay.record\",\"path\":\"demo.ceir\",\"args\":{\"executor\":\"plan\"}}\n"
                  "@{\"command\":\"replay.record\",\"path\":\"demo.ceir\",\"args\":{\"out\":\"../r.crrr\"}}\n"
                  "@{\"command\":\"replay.run\",\"path\":\"r.crrr\",\"args\":{\"program\":\"edited.ceir\"}}\n"
                  "@{\"command\":\"replay.run\",\"path\":\"r.crrr\",\"args\":{\"program\":\"../edited.ceir\"}}\n"
                  "@{\"command\":\"replay.run\",\"path\":\"r.crrr\",\"args\":{\"kernel_dir\":\"k\",\"jobs\":\"x\"}}\n"
                  "@{\"command\":\"gpu.resources\"}\n"
                  "!program.inspect|demo.ceir||||1|steps=into|steps=over\n"
                  "@{\"command\":\"program.inspect\",\"path\":\"demo.ceir\",\"args\":{\"steps\":\"into\","
                  "\"steps\":\"over\"}}\n");
    sink.add_text("everything-granted-no-root",
                  "#host read,record,inject,remote-enable,upload,process-memory,execute\n"
                  "@{\"command\":\"replay.run\",\"path\":\"r.crrr\"}\n"
                  "@{\"command\":\"jobs.waits\"}\n"
                  "@{\"command\":\"memory.allocators\"}\n"
                  "@{\"command\":\"diag.commands\",\"page_items\":1,\"page_bytes\":1024}\n");
    sink.add_text("grant-none", "#host none\n@{\"command\":\"diag.commands\"}\n");
}
