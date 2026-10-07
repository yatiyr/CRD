// crd-perf -- typed, bounded diagnostic commands. See crd/perf/diag_commands.hpp.

#include <crd/perf/diag_commands.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/doctor.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/profiler.hpp>

#include "json_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace crd::perf
{
namespace
{
using detail::append_json_escaped;
using detail::append_u64;

constexpr cont::StringView kClippedTail = ",\"clipped\":true";

// Bytes a finished object needs beyond its fields: the braces and the clipped marker.
constexpr crd::usize kObjectOverhead = 2U + kClippedTail.size();

constexpr crd::u64 kCursorOffsetMask = (1ULL << kDiagCursorOffsetBits) - 1ULL;

// A request path is relative, '/'-separated, and every component is a plain name: no empty, "." or ".." component,
// no drive, no backslash and nothing outside [A-Za-z0-9._-]. So a joined path can never leave the host's root.
[[nodiscard]] bool safe_relative_path(cont::StringView path) noexcept
{
    if (path.empty())
    {
        return false;
    }
    crd::usize start = 0U;
    while (start <= path.size())
    {
        crd::usize end = path.find('/', start);
        if (end == cont::StringView::npos)
        {
            end = path.size();
        }
        const cont::StringView part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
        {
            return false;
        }
        for (const char c : part)
        {
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '.' || c == '_' || c == '-';
            if (!plain)
            {
                return false;
            }
        }
        start = end + 1U;
    }
    return true;
}

[[nodiscard]] bool valid_command_name(cont::StringView name) noexcept
{
    if (name.empty() || name.size() > kDiagMaxCommandBytes)
    {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                                          c == '-';
                               });
}

[[nodiscard]] bool valid_arg_name(cont::StringView name) noexcept
{
    if (name.empty() || name.size() > kDiagMaxArgNameBytes)
    {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                               });
}

[[nodiscard]] bool single_known_authority(DiagAuthority a) noexcept
{
    const DiagAuthoritySet bits = authority_bit(a);
    return bits != 0U && (bits & (bits - 1U)) == 0U && (bits & ~kDiagAllAuthorities) == 0U;
}

[[nodiscard]] std::FILE* open_file(const char* path, const char* mode) noexcept
{
    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path, mode) != 0)
    {
        return nullptr;
    }
#else
    fp = std::fopen(path, mode);
#endif
    return fp;
}

// The file's size without reading it, or -1.
[[nodiscard]] crd::i64 file_size(std::FILE* fp) noexcept
{
#if defined(_MSC_VER)
    if (_fseeki64(fp, 0, SEEK_END) != 0)
    {
        return -1;
    }
    const crd::i64 size = _ftelli64(fp);
    if (_fseeki64(fp, 0, SEEK_SET) != 0)
    {
        return -1;
    }
#else
    if (fseeko(fp, 0, SEEK_END) != 0)
    {
        return -1;
    }
    const crd::i64 size = static_cast<crd::i64>(ftello(fp));
    if (fseeko(fp, 0, SEEK_SET) != 0)
    {
        return -1;
    }
#endif
    return size;
}

void append_hex(cont::String& out, const crd::u8* bytes, crd::usize n)
{
    static constexpr char kHex[] = "0123456789abcdef";
    for (crd::usize i = 0U; i < n; ++i)
    {
        out.push_back(kHex[(bytes[i] >> 4U) & 0xFU]);
        out.push_back(kHex[bytes[i] & 0xFU]);
    }
}

[[nodiscard]] cont::StringView section_tag_name(crd::u32 tag) noexcept
{
    switch (static_cast<BundleSectionTag>(tag))
    {
        case BundleSectionTag::Manifest:
            return "manifest";
        case BundleSectionTag::CrashRecord:
            return "crash-record";
        case BundleSectionTag::RawDump:
            return "raw-dump";
        case BundleSectionTag::LogTail:
            return "log-tail";
        case BundleSectionTag::SymbolIndex:
            return "symbol-index";
        case BundleSectionTag::Invalid:
            return "invalid";
    }
    return tag >= 0x00010000U ? "module" : "reserved";
}

[[nodiscard]] cont::StringView import_status_name(ImportStatus s) noexcept
{
    switch (s)
    {
        case ImportStatus::Ok:
            return "ok";
        case ImportStatus::RecoveredTruncated:
            return "recovered-truncated";
        case ImportStatus::Rejected:
            return "rejected";
    }
    return "unknown";
}

[[nodiscard]] cont::StringView import_reject_name(ImportReject r) noexcept
{
    switch (r)
    {
        case ImportReject::None:
            return "none";
        case ImportReject::Oversized:
            return "oversized";
        case ImportReject::BadMagic:
            return "bad-magic";
        case ImportReject::BadHeader:
            return "bad-header";
        case ImportReject::UnsupportedVersion:
            return "unsupported-version";
        case ImportReject::SchemaMismatch:
            return "schema-mismatch";
        case ImportReject::TooManySections:
            return "too-many-sections";
    }
    return "unknown";
}

[[nodiscard]] cont::StringView symbol_kind_name(SymbolIdKind k) noexcept
{
    switch (k)
    {
        case SymbolIdKind::None:
            return "none";
        case SymbolIdKind::Rsds:
            return "rsds";
        case SymbolIdKind::GnuBuildId:
            return "gnu-build-id";
        case SymbolIdKind::PeImage:
            return "pe-image";
    }
    return "unknown";
}

constexpr DiagAuthority kAuthorityOrder[kDiagAuthorityCount] = {
    DiagAuthority::Read,   DiagAuthority::Record,        DiagAuthority::Inject,  DiagAuthority::RemoteEnable,
    DiagAuthority::Upload, DiagAuthority::ProcessMemory, DiagAuthority::Execute,
};

} // namespace

bool diag_path_is_safe(cont::StringView path) noexcept
{
    return safe_relative_path(path);
}

// ---- names ----------------------------------------------------------------------------------------------------------

cont::StringView authority_name(DiagAuthority a) noexcept
{
    switch (a)
    {
        case DiagAuthority::None:
            return "none";
        case DiagAuthority::Read:
            return "read";
        case DiagAuthority::Record:
            return "record";
        case DiagAuthority::Inject:
            return "inject";
        case DiagAuthority::RemoteEnable:
            return "remote-enable";
        case DiagAuthority::Upload:
            return "upload";
        case DiagAuthority::ProcessMemory:
            return "process-memory";
        case DiagAuthority::Execute:
            return "execute";
    }
    return "unknown";
}

bool parse_authority_list(cont::StringView text, DiagAuthoritySet& out) noexcept
{
    if (text.empty())
    {
        return false;
    }
    if (text == "none")
    {
        out = 0U;
        return true;
    }
    DiagAuthoritySet set   = 0U;
    crd::usize       start = 0U;
    while (start <= text.size())
    {
        crd::usize end = text.find(',', start);
        if (end == cont::StringView::npos)
        {
            end = text.size();
        }
        const cont::StringView item  = text.substr(start, end - start);
        bool                   found = false;
        for (const DiagAuthority a : kAuthorityOrder)
        {
            if (item == authority_name(a))
            {
                set |= authority_bit(a);
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
        start = end + 1U;
    }
    out = set;
    return true;
}

void append_authority_list(cont::String& out, DiagAuthoritySet set)
{
    bool first = true;
    for (const DiagAuthority a : kAuthorityOrder)
    {
        if (grants(set, a))
        {
            if (!first)
            {
                out.push_back(',');
            }
            out.append(authority_name(a));
            first = false;
        }
    }
    if (first)
    {
        out.append("none");
    }
}

cont::StringView status_name(DiagStatus s) noexcept
{
    switch (s)
    {
        case DiagStatus::Ok:
            return "ok";
        case DiagStatus::UnsupportedSchema:
            return "unsupported-schema";
        case DiagStatus::UnknownCommand:
            return "unknown-command";
        case DiagStatus::Unauthorized:
            return "unauthorized";
        case DiagStatus::Oversized:
            return "oversized";
        case DiagStatus::BadArgument:
            return "bad-argument";
        case DiagStatus::StaleCursor:
            return "stale-cursor";
        case DiagStatus::Cancelled:
            return "cancelled";
        case DiagStatus::Unavailable:
            return "unavailable";
        case DiagStatus::Failed:
            return "failed";
    }
    return "unknown";
}

// ---- result, fields, snapshot ---------------------------------------------------------------------------------------

DiagResult::DiagResult(crd::memory::IAllocator* alloc) : reason(alloc), json(alloc) {}

DiagFields::DiagFields(crd::memory::IAllocator* alloc, crd::u32 max_bytes)
    : m_alloc(alloc), m_body(alloc), m_scratch(alloc), m_max_bytes(max_bytes)
{
}

void DiagFields::append_field(cont::StringView key, const cont::String& value_text)
{
    // ,"key":value
    const crd::usize add = (m_body.empty() ? 0U : 1U) + key.size() + 3U + value_text.size();
    if (m_body.size() + add + kObjectOverhead > m_max_bytes)
    {
        m_clipped = true;
        return;
    }
    if (!m_body.empty())
    {
        m_body.push_back(',');
    }
    m_body.push_back('"');
    m_body.append(key);
    m_body.append("\":");
    m_body.append(value_text);
}

DiagFields& DiagFields::str(cont::StringView key, cont::StringView value)
{
    cont::StringView kept = value;
    if (kept.size() > kDiagMaxFieldBytes)
    {
        crd::usize cut = kDiagMaxFieldBytes;
        // Never split a UTF-8 sequence: back off over continuation bytes.
        while (cut > 0U && (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U)
        {
            --cut;
        }
        kept      = value.substr(0U, cut);
        m_clipped = true;
    }
    m_scratch.clear();
    m_scratch.push_back('"');
    append_json_escaped(m_scratch, kept);
    m_scratch.push_back('"');
    append_field(key, m_scratch);
    return *this;
}

DiagFields& DiagFields::u64(cont::StringView key, crd::u64 value)
{
    m_scratch.clear();
    append_u64(m_scratch, value);
    append_field(key, m_scratch);
    return *this;
}

DiagFields& DiagFields::i64(cont::StringView key, crd::i64 value)
{
    m_scratch.clear();
    detail::append_i64(m_scratch, value);
    append_field(key, m_scratch);
    return *this;
}

DiagFields& DiagFields::boolean(cont::StringView key, bool value)
{
    m_scratch.clear();
    m_scratch.append(value ? "true" : "false");
    append_field(key, m_scratch);
    return *this;
}

cont::String DiagFields::object() const
{
    cont::String out(m_alloc);
    out.push_back('{');
    out.append(cont::StringView{m_body.data(), m_body.size()});
    if (m_clipped)
    {
        out.append(m_body.empty() ? kClippedTail.substr(1U) : kClippedTail);
    }
    out.push_back('}');
    return out;
}

void DiagFields::clear() noexcept
{
    m_body.clear();
    m_scratch.clear();
    m_clipped = false;
}

DiagSnapshot::DiagSnapshot(crd::memory::IAllocator* alloc)
    : summary(alloc, kDiagMaxItemBytes), reason(alloc), m_alloc(alloc), m_items(alloc)
{
}

bool DiagSnapshot::add_item(const DiagFields& item)
{
    if (m_items.size() >= kDiagMaxSnapshotItems)
    {
        ++m_dropped;
        return false;
    }
    m_items.push_back(item.object());
    return true;
}

void DiagSnapshot::clear() noexcept
{
    summary.clear();
    reason.clear();
    m_items.clear();
    m_dropped = 0U;
}

// ---- service --------------------------------------------------------------------------------------------------------

DiagCommandService::DiagCommandService(DiagAuthoritySet grant, const DiagServiceConfig& config,
                                       crd::memory::IAllocator* alloc)
    : m_alloc(alloc),
      m_grant(grant & kDiagAllAuthorities),
      m_root(config.root.data(), config.root.size(), alloc),
      m_config(config),
      m_commands(alloc),
      m_snapshot(alloc)
{
    m_config.root = cont::StringView{}; // the owned copy in m_root is the only root

    const Entry builtins[] = {
        {{"diag.commands", "perf", "every registered command with its owner, authority and path argument",
          DiagAuthority::Read, false},
         &DiagCommandService::run_commands, this},
        {{"diag.capabilities", "perf", "diagnostic modes and dependencies, the command schema and the grant",
          DiagAuthority::Read, false},
         &DiagCommandService::run_capabilities, this},
        {{"jobs.waits", "perf", "parked fibers with their wait edges, and each worker's responsiveness",
          DiagAuthority::Read, false},
         &DiagCommandService::run_jobs_waits, this},
        {{"memory.allocators", "perf", "allocators registered with the profiler and their live statistics",
          DiagAuthority::Read, false},
         &DiagCommandService::run_allocators, this},
        {{"bundle.inspect", "perf", "a post-mortem bundle under the root, through the bounded non-executing importer",
          DiagAuthority::Read, true},
         &DiagCommandService::run_bundle_inspect, this},
        {{"capture.start", "perf", "open a capture window (clears the profiler's sample rings)", DiagAuthority::Record,
          false},
         &DiagCommandService::run_capture_start, this},
        {{"capture.stop", "perf", "write the open window as a CPROF capture under the root, never overwriting",
          DiagAuthority::Record, true},
         &DiagCommandService::run_capture_stop, this},
    };
    for (const Entry& e : builtins)
    {
        m_commands.push_back(e);
    }
}

bool DiagCommandService::register_command(const DiagCommandSpec& spec, DiagHandler handler, void* context)
{
    return register_command(spec, handler, context, nullptr);
}

bool DiagCommandService::register_command(const DiagCommandSpec& spec, DiagHandler handler, void* context,
                                          DiagArgsCheck check)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const bool also_ok = spec.also == DiagAuthority::None ||
                         (single_known_authority(spec.also) && spec.also != spec.authority);
    if (handler == nullptr || !valid_command_name(spec.name) || !single_known_authority(spec.authority) || !also_ok ||
        m_commands.size() >= kDiagMaxCommands || find(spec.name) != kNone)
    {
        return false;
    }
    Entry e;
    e.spec    = spec;
    e.handler = handler;
    e.context = context;
    e.check   = check;
    m_commands.push_back(e);
    return true;
}

crd::u32 DiagCommandService::command_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<crd::u32>(m_commands.size());
}

const DiagCommandSpec* DiagCommandService::command_at(crd::u32 index) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return index < m_commands.size() ? &m_commands[index].spec : nullptr;
}

crd::u64 DiagCommandService::handler_runs() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_handler_runs;
}

crd::u64 DiagCommandService::file_bytes_read() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_file_bytes_read;
}

crd::u64 DiagCommandService::capture_window() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_capture_window;
}

bool DiagCommandService::capture_open() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_capture_open;
}

DiagStatus DiagCommandService::check_args(const Entry& entry, const DiagRequest& request, cont::String& reason) const
{
    if (request.args.empty())
    {
        return entry.check != nullptr ? entry.check(entry.context, request.args, reason) : DiagStatus::Ok;
    }
    if (entry.check == nullptr)
    {
        reason.append("the command takes no arguments");
        return DiagStatus::BadArgument;
    }
    for (crd::usize i = 0U; i < request.args.size(); ++i)
    {
        const cont::StringView name = request.args[i].name;
        if (!valid_arg_name(name))
        {
            reason.append("an argument name must be 1 to ");
            append_u64(reason, kDiagMaxArgNameBytes);
            reason.append(" bytes of [a-z0-9_]");
            return DiagStatus::BadArgument;
        }
        for (crd::usize k = 0U; k < i; ++k)
        {
            if (request.args[k].name == name)
            {
                reason.append("the argument '");
                reason.append(name);
                reason.append("' is given more than once");
                return DiagStatus::BadArgument;
            }
        }
    }
    const DiagStatus status = entry.check(entry.context, request.args, reason);
    if (status != DiagStatus::Ok && reason.empty())
    {
        reason.append("the command refused its arguments");
    }
    return status;
}

crd::u32 DiagCommandService::find(cont::StringView name) const noexcept
{
    for (crd::usize i = 0U; i < m_commands.size(); ++i)
    {
        if (m_commands[i].spec.name == name)
        {
            return static_cast<crd::u32>(i);
        }
    }
    return kNone;
}

namespace
{
// The fields every response document starts with.
void begin_document(cont::String& json, const DiagRequest& request, DiagStatus status, cont::StringView reason,
                    DiagAuthoritySet grant)
{
    json.append(R"({"schema":)");
    append_u64(json, kDiagCommandSchemaVersion);
    json.append(R"(,"command":")");
    append_json_escaped(json, request.command.substr(0U, kDiagMaxCommandBytes));
    json.append(R"(","ok":)");
    json.append(status == DiagStatus::Ok ? "true" : "false");
    json.append(R"(,"status":")");
    json.append(status_name(status));
    json.append(R"(","reason":")");
    append_json_escaped(json, reason);
    json.append(R"(","grant":")");
    append_authority_list(json, grant);
    json.push_back('"');
}

void append_number(cont::String& json, cont::StringView key, crd::u64 value)
{
    json.append(",\"");
    json.append(key);
    json.append("\":");
    append_u64(json, value);
}

void append_flag(cont::String& json, cont::StringView key, bool value)
{
    json.append(",\"");
    json.append(key);
    json.append("\":");
    json.append(value ? "true" : "false");
}
} // namespace

void DiagCommandService::refuse(DiagResult& result, const DiagRequest& request, DiagStatus status,
                                cont::StringView reason) const
{
    result.status = status;
    result.reason.clear();
    result.reason.append(reason);
    result.json.clear();
    begin_document(result.json, request, status, reason, m_grant);
    append_number(result.json, "generation", 0U);
    append_number(result.json, "cursor", request.cursor);
    append_number(result.json, "next_cursor", 0U);
    append_number(result.json, "total", 0U);
    append_number(result.json, "page_items", 0U);
    append_number(result.json, "dropped", 0U);
    append_flag(result.json, "complete", false);
    append_flag(result.json, "byte_bounded", false);
    result.json.append(R"(,"summary":{},"items":[]})");
}

void DiagCommandService::page(DiagResult& result, const DiagRequest& request, crd::u64 offset) const
{
    const cont::Array<cont::String>& items     = m_snapshot.items();
    const crd::u32                   max_items = request.page_items == 0U ? kDiagDefaultPageItems : request.page_items;
    const crd::u32                   max_bytes = request.page_bytes == 0U ? kDiagDefaultPageBytes : request.page_bytes;
    const crd::u64                   total     = items.size();

    crd::u64 end   = offset;
    crd::u64 bytes = 0U;
    bool     byte_bounded = false;
    while (end < total && end - offset < max_items)
    {
        const crd::u64 cost = items[static_cast<crd::usize>(end)].size() + (end > offset ? 1U : 0U);
        if (bytes + cost > max_bytes)
        {
            byte_bounded = true;
            break;
        }
        bytes += cost;
        ++end;
    }

    const crd::u64 base   = m_generation << kDiagCursorOffsetBits;
    result.status         = DiagStatus::Ok;
    result.reason.clear();
    result.generation     = m_generation;
    result.cursor         = base | offset;
    result.next_cursor    = end < total ? (base | end) : 0U;
    result.total          = static_cast<crd::u32>(total);
    result.items          = static_cast<crd::u32>(end - offset);
    result.dropped        = m_snapshot.dropped();
    result.complete       = end == total;
    result.byte_bounded   = byte_bounded;

    result.json.clear();
    begin_document(result.json, request, DiagStatus::Ok, cont::StringView{}, m_grant);
    append_number(result.json, "generation", result.generation);
    append_number(result.json, "cursor", result.cursor);
    append_number(result.json, "next_cursor", result.next_cursor);
    append_number(result.json, "total", result.total);
    append_number(result.json, "page_items", result.items);
    append_number(result.json, "dropped", result.dropped);
    append_flag(result.json, "complete", result.complete);
    append_flag(result.json, "byte_bounded", result.byte_bounded);
    result.json.append(R"(,"summary":)");
    result.json.append(cont::StringView{m_snapshot.summary.object()});
    result.json.append(R"(,"items":[)");
    for (crd::u64 i = offset; i < end; ++i)
    {
        if (i > offset)
        {
            result.json.push_back(',');
        }
        const cont::String& item = items[static_cast<crd::usize>(i)];
        result.json.append(cont::StringView{item.data(), item.size()});
    }
    result.json.append(R"(]})");
}

DiagResult DiagCommandService::execute(const DiagRequest& request, const std::atomic<bool>* cancel)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    DiagResult                        result(m_alloc);
    cont::String                      reason(m_alloc);

    if (request.schema_version != kDiagCommandSchemaVersion)
    {
        reason.append("request schema ");
        append_u64(reason, request.schema_version);
        reason.append(" is not this service's schema ");
        append_u64(reason, kDiagCommandSchemaVersion);
        refuse(result, request, DiagStatus::UnsupportedSchema, reason);
        return result;
    }

    const crd::u32 index = find(request.command);
    if (index == kNone)
    {
        refuse(result, request, DiagStatus::UnknownCommand, "no command of that name is registered");
        return result;
    }
    const Entry& entry = m_commands[index];

    for (const DiagAuthority needed : {entry.spec.authority, entry.spec.also})
    {
        if (needed != DiagAuthority::None && !grants(m_grant, needed))
        {
            reason.append("the command needs ");
            reason.append(authority_name(needed));
            reason.append(" authority; the host granted ");
            append_authority_list(reason, m_grant);
            refuse(result, request, DiagStatus::Unauthorized, reason);
            return result;
        }
    }

    if (request.page_items > kDiagMaxPageItems)
    {
        reason.append("page_items is above ");
        append_u64(reason, kDiagMaxPageItems);
        refuse(result, request, DiagStatus::Oversized, reason);
        return result;
    }
    if (request.page_bytes > kDiagMaxPageBytes)
    {
        reason.append("page_bytes is above ");
        append_u64(reason, kDiagMaxPageBytes);
        refuse(result, request, DiagStatus::Oversized, reason);
        return result;
    }
    if (request.path.size() > kDiagMaxPathBytes)
    {
        reason.append("path is longer than ");
        append_u64(reason, kDiagMaxPathBytes);
        reason.append(" bytes");
        refuse(result, request, DiagStatus::Oversized, reason);
        return result;
    }
    if (request.args.size() > kDiagMaxArgs)
    {
        reason.append("more than ");
        append_u64(reason, kDiagMaxArgs);
        reason.append(" arguments");
        refuse(result, request, DiagStatus::Oversized, reason);
        return result;
    }
    for (const DiagArg& a : request.args)
    {
        if (a.name.size() > kDiagMaxArgNameBytes || a.value.size() > kDiagMaxArgValueBytes)
        {
            reason.append("an argument name is longer than ");
            append_u64(reason, kDiagMaxArgNameBytes);
            reason.append(" bytes or its value longer than ");
            append_u64(reason, kDiagMaxArgValueBytes);
            refuse(result, request, DiagStatus::Oversized, reason);
            return result;
        }
    }

    if (request.page_bytes != 0U && request.page_bytes < kDiagMinPageBytes)
    {
        reason.append("page_bytes is below ");
        append_u64(reason, kDiagMinPageBytes);
        refuse(result, request, DiagStatus::BadArgument, reason);
        return result;
    }
    if (!entry.spec.takes_path && !request.path.empty())
    {
        refuse(result, request, DiagStatus::BadArgument, "the command takes no path");
        return result;
    }
    if (entry.spec.takes_path)
    {
        if (request.path.empty())
        {
            refuse(result, request, DiagStatus::BadArgument, "the command needs a path");
            return result;
        }
        if (!safe_relative_path(request.path))
        {
            refuse(result, request, DiagStatus::BadArgument,
                   "the path must be relative and made of plain [A-Za-z0-9._-] names separated by '/'");
            return result;
        }
        if (m_root.empty())
        {
            refuse(result, request, DiagStatus::Unavailable, "the host granted no file root");
            return result;
        }
    }
    if (const DiagStatus args = check_args(entry, request, reason); args != DiagStatus::Ok)
    {
        refuse(result, request, args, reason);
        return result;
    }

    crd::u64 offset = 0U;
    if (request.cursor != 0U)
    {
        const crd::u64 generation = request.cursor >> kDiagCursorOffsetBits;
        offset                    = request.cursor & kCursorOffsetMask;
        if (generation == 0U || generation != m_generation || m_snapshot_of != index)
        {
            refuse(result, request, DiagStatus::StaleCursor,
                   "the cursor names a snapshot this service no longer retains");
            return result;
        }
        if (offset > m_snapshot.items().size())
        {
            refuse(result, request, DiagStatus::BadArgument, "the cursor's offset is past the snapshot");
            return result;
        }
    }

    if (cancel != nullptr && cancel->load(std::memory_order_acquire))
    {
        refuse(result, request, DiagStatus::Cancelled, "the request was cancelled before it started");
        return result;
    }

    if (request.cursor != 0U)
    {
        page(result, request, offset);
        return result;
    }

    if (m_generation >= kDiagMaxGeneration)
    {
        refuse(result, request, DiagStatus::Failed, "the snapshot generation space is exhausted");
        return result;
    }

    // A new snapshot replaces the retained one, so every earlier cursor is stale from here on.
    m_snapshot.clear();
    m_snapshot_of = kNone;
    ++m_handler_runs;

    cont::String file(m_alloc);
    if (entry.spec.takes_path)
    {
        file.append(cont::StringView{m_root.data(), m_root.size()});
        const char last = m_root.data()[m_root.size() - 1U];
        if (last != '/' && last != '\\')
        {
            file.push_back('/');
        }
        file.append(request.path);
    }

    DiagCall call;
    call.request = &request;
    call.file    = cont::StringView{file.data(), file.size()};
    call.root    = cont::StringView{m_root.data(), m_root.size()};
    call.cancel  = cancel;
    call.granted = m_grant;

    const DiagStatus status = entry.handler(entry.context, call, m_snapshot);
    if (status != DiagStatus::Ok)
    {
        reason.append(cont::StringView{m_snapshot.reason.data(), m_snapshot.reason.size()});
        if (reason.empty())
        {
            reason.append("the command reported ");
            reason.append(status_name(status));
        }
        m_snapshot.clear();
        refuse(result, request, status, reason);
        return result;
    }

    ++m_generation;
    m_snapshot_of = index;
    page(result, request, 0U);
    return result;
}

// ---- built-in commands ----------------------------------------------------------------------------------------------

DiagStatus DiagCommandService::run_commands(void* self, const DiagCall& call, DiagSnapshot& out)
{
    const auto*  svc     = static_cast<const DiagCommandService*>(self);
    crd::u32     granted = 0U;
    DiagFields   item(out.allocator());
    for (const Entry& e : svc->m_commands)
    {
        const bool ok = grants(call.granted, e.spec.authority) &&
                        (e.spec.also == DiagAuthority::None || grants(call.granted, e.spec.also));
        granted += ok ? 1U : 0U;
        item.clear();
        item.str("name", e.spec.name).str("owner", e.spec.owner).str("authority", authority_name(e.spec.authority));
        if (e.spec.also != DiagAuthority::None)
        {
            item.str("also", authority_name(e.spec.also));
        }
        item.boolean("granted", ok)
            .boolean("takes_path", e.spec.takes_path)
            .boolean("takes_args", e.check != nullptr)
            .str("summary", e.spec.summary);
        (void)out.add_item(item);
    }
    out.summary.u64("count", svc->m_commands.size()).u64("granted", granted);
    return DiagStatus::Ok;
}

DiagStatus DiagCommandService::run_capabilities(void* self, const DiagCall& call, DiagSnapshot& out)
{
    const auto*        svc    = static_cast<const DiagCommandService*>(self);
    const DoctorReport report = run_doctor();

    cont::String grant(out.allocator());
    append_authority_list(grant, call.granted);
    out.summary.str("schema", report.schema)
        .str("host", cont::StringView{report.host_tuple.data(), report.host_tuple.size()})
        .u64("command_schema", kDiagCommandSchemaVersion)
        .str("grant", cont::StringView{grant.data(), grant.size()})
        .boolean("profiling_compiled", CRD_PERF_ENABLED != 0)
        .boolean("profiler_active", is_active())
        .boolean("job_pool", crd::jobs::num_workers() > 0U)
        .boolean("file_root", !svc->m_root.empty());

    DiagFields item(out.allocator());
    for (const DiagAuthority a : kAuthorityOrder)
    {
        item.clear();
        item.str("kind", "authority").str("name", authority_name(a)).boolean("granted", grants(call.granted, a));
        (void)out.add_item(item);
    }
    for (const DiagnosticMode& m : report.modes)
    {
        item.clear();
        item.str("kind", "mode")
            .str("name", m.name)
            .boolean("compiled", m.compiled)
            .boolean("enabled", m.enabled)
            .boolean("usable", m.usable)
            .str("disposition", m.disposition);
        (void)out.add_item(item);
    }
    for (const DiagnosticDependency& d : report.dependencies)
    {
        item.clear();
        item.str("kind", "dependency")
            .str("name", d.name)
            .boolean("present", d.present)
            .str("detail", cont::StringView{d.detail.data(), d.detail.size()});
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
}

DiagStatus DiagCommandService::run_jobs_waits(void* self, const DiagCall& call, DiagSnapshot& out)
{
    const auto*    svc     = static_cast<const DiagCommandService*>(self);
    const crd::u32 workers = crd::jobs::num_workers();
    if (workers == 0U)
    {
        out.reason.append("the job pool is not initialised in this process");
        return DiagStatus::Unavailable;
    }

    constexpr crd::usize parked_cap = 1024U;
    cont::Array<crd::jobs::WaitGraphNode> parked(out.allocator());
    parked.resize(parked_cap);
    const crd::usize parked_total  = crd::jobs::wait_graph_snapshot({parked.data(), parked.size()});
    const crd::usize parked_listed = parked_total < parked_cap ? parked_total : parked_cap;
    if (call.cancelled())
    {
        out.reason.append("cancelled after the wait-graph snapshot");
        return DiagStatus::Cancelled;
    }

    cont::Array<crd::jobs::WorkerNode> nodes(out.allocator());
    nodes.resize(workers);
    const crd::jobs::WorkerSnapshotResult ws =
        crd::jobs::worker_snapshot({nodes.data(), nodes.size()}, svc->m_config.worker_timeout_ms);
    const crd::usize workers_listed = ws.total < nodes.size() ? ws.total : nodes.size();

    out.summary.u64("parked", parked_total)
        .u64("parked_listed", parked_listed)
        .u64("workers", ws.total)
        .u64("polled", ws.expected)
        .u64("responded", ws.responded)
        .boolean("complete", ws.complete && parked_listed == parked_total)
        .u64("timeout_ms", svc->m_config.worker_timeout_ms);

    DiagFields item(out.allocator());
    for (crd::usize i = 0U; i < parked_listed; ++i)
    {
        const crd::jobs::WaitGraphNode& n = parked[i];
        item.clear();
        item.str("kind", "parked")
            .u64("fiber", n.fiber_index)
            .u64("tier", n.tier)
            .u64("own_task", n.own_task_id)
            .u64("waiting_on", n.waiting_on_task_id)
            .u64("waiting_on_parent", n.waiting_on_parent_task_id)
            .u64("remaining", n.waiting_on_remaining);
        (void)out.add_item(item);
    }
    for (crd::usize i = 0U; i < workers_listed; ++i)
    {
        const crd::jobs::WorkerNode& w = nodes[i];
        item.clear();
        item.str("kind", "worker")
            .u64("thread", w.thread_index)
            .u64("task", w.current_task_id)
            .boolean("executing", w.executing)
            .boolean("responsive", w.responsive);
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
}

DiagStatus DiagCommandService::run_allocators(void* self, const DiagCall& call, DiagSnapshot& out)
{
    (void)self;
    (void)call;
#if !CRD_PERF_ENABLED
    out.reason.append("profiling is compiled out of this build (CRD_ENABLE_PROFILING is off)");
    return DiagStatus::Unavailable;
#else
    if (!is_active())
    {
        out.reason.append("the profiler is not initialised in this process");
        return DiagStatus::Unavailable;
    }
    const crd::u32 slots = registered_allocator_count();
    out.summary.u64("slots", slots).u64("live", live_allocator_count());
    DiagFields item(out.allocator());
    for (crd::u32 i = 0U; i < slots; ++i)
    {
        const AllocatorInfo info = allocator_info(i);
        if (info.allocator == nullptr)
        {
            continue;
        }
        const AllocatorSnapshot s = allocator_snapshot(i);
        item.clear();
        item.u64("index", i)
            .str("name", info.name != nullptr ? cont::StringView{info.name} : cont::StringView{})
            .u64("allocations", s.alloc_count)
            .u64("deallocations", s.dealloc_count)
            .u64("bytes_in_use", s.bytes_in_use)
            .u64("peak_bytes", s.peak_bytes)
            .u64("total_bytes", s.total_bytes);
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
#endif
}

DiagStatus DiagCommandService::run_bundle_inspect(void* self, const DiagCall& call, DiagSnapshot& out)
{
    auto* svc = static_cast<DiagCommandService*>(self);

    cont::String path(call.file.data(), call.file.size(), out.allocator());
    std::FILE*   fp = open_file(path.c_str(), "rb");
    if (fp == nullptr)
    {
        out.reason.append("cannot open the bundle");
        return DiagStatus::Failed;
    }
    const crd::i64 size = file_size(fp);
    if (size < 0)
    {
        (void)std::fclose(fp);
        out.reason.append("cannot size the bundle");
        return DiagStatus::Failed;
    }
    // The bound is checked on the file's size, before a byte is read.
    if (static_cast<crd::u64>(size) > svc->m_config.max_bundle_bytes)
    {
        (void)std::fclose(fp);
        out.reason.append("the bundle is ");
        append_u64(out.reason, static_cast<crd::u64>(size));
        out.reason.append(" bytes; the host's limit is ");
        append_u64(out.reason, svc->m_config.max_bundle_bytes);
        return DiagStatus::Oversized;
    }

    cont::Array<crd::u8> bytes(out.allocator());
    bytes.resize(static_cast<crd::usize>(size));
    const crd::usize read = size > 0 ? std::fread(bytes.data(), 1U, bytes.size(), fp) : 0U;
    (void)std::fclose(fp);
    svc->m_file_bytes_read += read;
    if (read != bytes.size())
    {
        out.reason.append("short read of the bundle");
        return DiagStatus::Failed;
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled after the bundle was read");
        return DiagStatus::Cancelled;
    }

    BundleLimits limits;
    limits.max_total_bytes = svc->m_config.max_bundle_bytes;
    const BundleImport imp = import_bundle({bytes.data(), bytes.size()}, limits, out.allocator());

    out.summary.str("path", call.request->path)
        .u64("bytes", bytes.size())
        .str("status", import_status_name(imp.status))
        .str("reject", import_reject_name(imp.reject))
        .u64("format_version", imp.status == ImportStatus::Rejected ? 0U : imp.header.format_version)
        .u64("schema_version", imp.status == ImportStatus::Rejected ? 0U : imp.header.schema_version)
        .u64("declared_sections", imp.declared_section_count)
        .u64("sections", imp.sections.size())
        .boolean("total_len_mismatch", imp.total_len_mismatch)
        .boolean("compressed_unsupported", imp.compressed_unsupported)
        .boolean("manifest", imp.has_manifest && imp.manifest.ok)
        .u64("manifest_absent_mismatches", imp.manifest_absent_mismatches)
        .u64("symbols", imp.symbols.size())
        .u64("unsafe_names", imp.unsafe_names);

    DiagFields item(out.allocator());
    for (crd::usize i = 0U; i < imp.sections.size(); ++i)
    {
        const ImportedSection& s   = imp.sections[i];
        const crd::u32         tag = static_cast<crd::u32>(s.tag);
        item.clear();
        item.str("kind", "section")
            .u64("index", i)
            .u64("tag", tag)
            .str("tag_name", section_tag_name(tag))
            .boolean("absent", (s.section_flags & kSectionFlagAbsent) != 0U)
            .boolean("truncated", (s.section_flags & kSectionFlagTruncated) != 0U)
            .boolean("compressed", (s.section_flags & kSectionFlagCompressed) != 0U)
            .boolean("oversized", (s.import_flags & kImportSectionOversized) != 0U)
            .boolean("duplicate", (s.import_flags & kImportSectionDuplicate) != 0U)
            .u64("original_length", s.original_length)
            .u64("payload_length", s.payload.size());
        (void)out.add_item(item);
    }
    cont::String id(out.allocator());
    for (const ModuleIdentity& m : imp.symbols)
    {
        id.clear();
        append_hex(id, m.id, m.id_len <= kMaxSymbolIdBytes ? m.id_len : kMaxSymbolIdBytes);
        item.clear();
        item.str("kind", "symbol")
            .str("name", cont::StringView{m.name.data(), m.name.size()})
            .str("debug_file", cont::StringView{m.debug_file.data(), m.debug_file.size()})
            .str("id_kind", symbol_kind_name(m.id_kind))
            .str("id", cont::StringView{id.data(), id.size()})
            .u64("age", m.age)
            .u64("image_bytes", m.size)
            .boolean("unloaded", m.unloaded);
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
}

DiagStatus DiagCommandService::run_capture_start(void* self, const DiagCall& call, DiagSnapshot& out)
{
    (void)call;
    auto* svc = static_cast<DiagCommandService*>(self);
#if !CRD_PERF_ENABLED
    (void)svc;
    out.reason.append("profiling is compiled out of this build (CRD_ENABLE_PROFILING is off)");
    return DiagStatus::Unavailable;
#else
    if (!is_active())
    {
        out.reason.append("the profiler is not initialised in this process");
        return DiagStatus::Unavailable;
    }
    if (svc->m_capture_open)
    {
        out.reason.append("capture window ");
        append_u64(out.reason, svc->m_capture_window);
        out.reason.append(" is already open");
        return DiagStatus::Failed;
    }
    clear_samples();
    svc->m_capture_open = true;
    ++svc->m_capture_window;
    out.summary.u64("window", svc->m_capture_window)
        .u64("threads", thread_count())
        .u64("ring_capacity", per_thread_ring_capacity());
    return DiagStatus::Ok;
#endif
}

DiagStatus DiagCommandService::run_capture_stop(void* self, const DiagCall& call, DiagSnapshot& out)
{
    auto* svc = static_cast<DiagCommandService*>(self);
#if !CRD_PERF_ENABLED
    (void)svc;
    (void)call;
    out.reason.append("profiling is compiled out of this build (CRD_ENABLE_PROFILING is off)");
    return DiagStatus::Unavailable;
#else
    if (!is_active())
    {
        out.reason.append("the profiler is not initialised in this process");
        return DiagStatus::Unavailable;
    }
    if (!svc->m_capture_open)
    {
        out.reason.append("no capture window is open");
        return DiagStatus::Failed;
    }
    cont::String path(call.file.data(), call.file.size(), out.allocator());
    if (std::FILE* existing = open_file(path.c_str(), "rb"); existing != nullptr)
    {
        (void)std::fclose(existing);
        out.reason.append("refusing to overwrite an existing file");
        return DiagStatus::Failed;
    }

    const crd::u64             contended_before = capture_contended_thread_count();
    const cont::Array<crd::u8> buf              = save_capture_to_buffer(out.allocator());
    const crd::u64             contended        = capture_contended_thread_count() - contended_before;
    if (buf.empty())
    {
        out.reason.append("the profiler produced no capture");
        return DiagStatus::Failed;
    }
    if (buf.size() > svc->m_config.max_capture_bytes)
    {
        out.reason.append("the capture is ");
        append_u64(out.reason, buf.size());
        out.reason.append(" bytes; the host's limit is ");
        append_u64(out.reason, svc->m_config.max_capture_bytes);
        out.reason.append(" (the window stays open)");
        return DiagStatus::Oversized;
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled before the capture was written (the window stays open)");
        return DiagStatus::Cancelled;
    }

    // "x": create only; a file that appeared since the check above is not overwritten either.
    std::FILE* fp = open_file(path.c_str(), "wbx");
    if (fp == nullptr)
    {
        out.reason.append("cannot create the capture file");
        return DiagStatus::Failed;
    }
    const crd::usize written = std::fwrite(buf.data(), 1U, buf.size(), fp);
    const bool       closed  = std::fclose(fp) == 0;
    if (written != buf.size() || !closed)
    {
        (void)std::remove(path.c_str());
        out.reason.append("short write of the capture file");
        return DiagStatus::Failed;
    }

    svc->m_capture_open = false;
    out.summary.str("path", call.request->path)
        .u64("window", svc->m_capture_window)
        .u64("bytes", buf.size())
        .boolean("valid", validate_capture_buffer({buf.data(), buf.size()}))
        .u64("contended_threads", contended)
        .boolean("samples_complete", contended == 0U);
    return DiagStatus::Ok;
#endif
}

} // namespace crd::perf
