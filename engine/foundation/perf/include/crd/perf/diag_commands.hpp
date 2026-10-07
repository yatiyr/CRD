#pragma once

// crd-perf -- typed, bounded diagnostic commands.
//
// One service a host composes and every consumer calls. A native caller, the ceridc CLI verb and its MCP tool all run
// DiagCommandService::execute and receive the same typed result and the same response bytes; a transport only moves
// the request in and the bytes out. The service needs no network, no MCP and no transport at all.
//
// Authority is granted by the host when it constructs the service and never travels in a request, so neither a request
// payload nor an authored asset can raise it. Each command declares the one authority it needs. Read and Record are
// the only classes a built-in command uses; Inject, RemoteEnable, Upload and ProcessMemory are distinct bits no other
// grant implies, so a host that grants Read and Record still refuses a command declaring any of them.
//
// Every refusal happens before the command's work, in a fixed order: schema version, unknown command, authority,
// request bounds (Oversized), arguments (BadArgument, or Unavailable when the host granted no file root), stale
// cursor, cancellation. Only then does the handler take its snapshot. A refusal leaves the retained snapshot alone.
//
// Pagination is deterministic. A request with cursor 0 runs the handler once and retains its snapshot under a new
// generation; every later page names that generation in its cursor and is cut from the retained snapshot, so the same
// cursor and bounds always return the same bytes. A cursor from a replaced snapshot (another command's, or an earlier
// snapshot of the same command) is refused StaleCursor. A page is bounded by an item count and by the serialized size
// of its items; each item is itself bounded below the smallest page, so every page makes progress.
//
// Upper modules (CEIR program provenance, replay preparation) register their own commands through register_command,
// so foundation never includes their headers. Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>
#include <mutex>

namespace crd::perf
{
namespace cont = crd::containers;

// Response and request schema version. A request naming another version is refused before any work.
inline constexpr crd::u32 kDiagCommandSchemaVersion = 1U;

// One authority class per bit. A grant is a set of them; no bit implies another.
enum class DiagAuthority : crd::u8
{
    None          = 0U,
    Read          = 1U << 0U, // snapshots and inspection of evidence that already exists
    Record        = 1U << 1U, // starting or stopping recording, and writing new evidence files
    Inject        = 1U << 2U, // fault injection (no built-in command)
    RemoteEnable  = 1U << 3U, // enabling diagnostics for a remote party (no built-in command)
    Upload        = 1U << 4U, // sending evidence off the machine (no built-in command)
    ProcessMemory = 1U << 5U, // reading arbitrary process memory (no built-in command)
};

using DiagAuthoritySet = crd::u32;

inline constexpr DiagAuthoritySet kDiagAllAuthorities = 0x3FU;
inline constexpr crd::u32         kDiagAuthorityCount = 6U;

[[nodiscard]] constexpr DiagAuthoritySet authority_bit(DiagAuthority a) noexcept
{
    return static_cast<DiagAuthoritySet>(a);
}

[[nodiscard]] constexpr bool grants(DiagAuthoritySet set, DiagAuthority a) noexcept
{
    return a != DiagAuthority::None && (set & authority_bit(a)) == authority_bit(a);
}

// "read", "record", "inject", "remote-enable", "upload", "process-memory" ("none" for None).
[[nodiscard]] cont::StringView authority_name(DiagAuthority a) noexcept;

// Parse a host's comma-separated grant ("read,record"; "none" is the empty set). Returns false, leaving `out`
// untouched, on an empty list, an empty item or an unknown name.
[[nodiscard]] bool parse_authority_list(cont::StringView text, DiagAuthoritySet& out) noexcept;

// Append the granted names, comma-separated in bit order ("none" for the empty set).
void append_authority_list(cont::String& out, DiagAuthoritySet set);

enum class DiagStatus : crd::u8
{
    Ok,
    UnsupportedSchema, // the request's schema version is not this service's
    UnknownCommand,    // no command of that name is registered
    Unauthorized,      // the host's grant lacks the command's authority
    Oversized,         // a request bound (page items, page bytes, path) or an input file exceeds its limit
    BadArgument,       // a missing, unexpected or unsafe argument
    StaleCursor,       // the cursor names a snapshot this service no longer retains
    Cancelled,         // the caller's cancel flag was raised
    Unavailable,       // the evidence cannot exist in this process or build; the reason says why
    Failed,            // the command started and could not finish (an I/O error, a state conflict); the reason says why
};

// "ok", "unsupported-schema", "unknown-command", "unauthorized", "oversized", "bad-argument", "stale-cursor",
// "cancelled", "unavailable", "failed".
[[nodiscard]] cont::StringView status_name(DiagStatus s) noexcept;

// Request bounds. 0 in a request field means the default.
inline constexpr crd::u32 kDiagDefaultPageItems = 32U;
inline constexpr crd::u32 kDiagMaxPageItems     = 256U;
inline constexpr crd::u32 kDiagDefaultPageBytes = 16U * 1024U;
inline constexpr crd::u32 kDiagMaxPageBytes     = 256U * 1024U;
inline constexpr crd::u32 kDiagMinPageBytes     = 1024U; // a smaller bound is BadArgument: every item must fit
inline constexpr crd::u32 kDiagMaxItemBytes     = 1000U; // an item stops taking fields here and says "clipped"
inline constexpr crd::u32 kDiagMaxFieldBytes    = 160U;  // a string value longer than this is clipped (and says so)
inline constexpr crd::u32 kDiagMaxPathBytes     = 256U;
inline constexpr crd::u32 kDiagMaxCommandBytes  = 64U;
inline constexpr crd::u32 kDiagMaxCommands      = 32U;

// A cursor is generation * 2^20 + offset, so it stays exact as a JSON number (below 2^53).
inline constexpr crd::u32 kDiagCursorOffsetBits = 20U;
inline constexpr crd::u32 kDiagMaxSnapshotItems = 1U << kDiagCursorOffsetBits;
inline constexpr crd::u64 kDiagMaxGeneration    = (1ULL << 33U) - 1U;

struct DiagRequest
{
    crd::u32         schema_version = kDiagCommandSchemaVersion;
    cont::StringView command;
    cont::StringView path;            // relative to the host's file root; only commands that declare a path take one
    crd::u64         cursor     = 0U; // 0 starts a new snapshot; otherwise a previous page's next_cursor
    crd::u32         page_items = 0U; // 0 = kDiagDefaultPageItems
    crd::u32         page_bytes = 0U; // 0 = kDiagDefaultPageBytes; bounds the serialized items of one page
};

// One page of one command's answer. `json` is the whole response document and is what every transport returns.
struct DiagResult
{
    explicit DiagResult(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    DiagStatus   status = DiagStatus::Failed;
    cont::String reason;             // why, for every status but Ok
    crd::u64     generation   = 0U;  // snapshot this page was cut from (0 when refused)
    crd::u64     cursor       = 0U;  // the cursor this page answers (the snapshot's first page for a cursor-0 request)
    crd::u64     next_cursor  = 0U;  // 0 when this page reaches the end
    crd::u32     total        = 0U;  // items in the snapshot
    crd::u32     items        = 0U;  // items on this page
    crd::u32     dropped      = 0U;  // items the snapshot could not keep (kDiagMaxSnapshotItems)
    bool         complete     = false; // this page reaches the end of the snapshot
    bool         byte_bounded = false; // the page ended early on its byte bound
    cont::String json;
};

// Builds the fields of one JSON object with a fixed key order. A string value is escaped and clipped to
// kDiagMaxFieldBytes; a field that would take the object past its byte bound is left out. Either way the finished
// object gains "clipped":true, so the result stays valid JSON and says it is incomplete.
class DiagFields
{
public:
    explicit DiagFields(crd::memory::IAllocator* alloc = crd::memory::default_allocator(),
                        crd::u32                 max_bytes = kDiagMaxItemBytes);

    DiagFields& str(cont::StringView key, cont::StringView value);
    DiagFields& u64(cont::StringView key, crd::u64 value);
    DiagFields& i64(cont::StringView key, crd::i64 value);
    DiagFields& boolean(cont::StringView key, bool value);

    // The object text ("{...}"), never longer than the byte bound.
    [[nodiscard]] cont::String object() const;
    [[nodiscard]] bool         clipped() const noexcept { return m_clipped; }
    void                       clear() noexcept;

private:
    void append_field(cont::StringView key, const cont::String& value_text);

    crd::memory::IAllocator* m_alloc;
    cont::String             m_body;    // the fields, comma-separated, without braces
    cont::String             m_scratch; // one value's text while it is checked against the bound
    crd::u32                 m_max_bytes = kDiagMaxItemBytes;
    bool                     m_clipped   = false;
};

// What a handler produces once per snapshot: summary fields and an ordered list of items.
class DiagSnapshot
{
public:
    explicit DiagSnapshot(crd::memory::IAllocator* alloc);

    DiagFields   summary;
    cont::String reason; // set with a non-Ok handler status

    // Keep one item's object text. Returns false and counts the drop once the snapshot holds kDiagMaxSnapshotItems.
    bool add_item(const DiagFields& item);

    [[nodiscard]] crd::u32                         dropped() const noexcept { return m_dropped; }
    [[nodiscard]] const cont::Array<cont::String>& items() const noexcept { return m_items; }
    [[nodiscard]] crd::memory::IAllocator*         allocator() const noexcept { return m_alloc; }
    void                                           clear() noexcept;

private:
    crd::memory::IAllocator*  m_alloc;
    cont::Array<cont::String> m_items;
    crd::u32                  m_dropped = 0U;
};

// The arguments a handler sees after every check passed.
struct DiagCall
{
    const DiagRequest*       request = nullptr;
    cont::StringView         file;    // the request path joined to the host's root (empty when the command takes none)
    const std::atomic<bool>* cancel  = nullptr;
    DiagAuthoritySet         granted = 0U;

    [[nodiscard]] bool cancelled() const noexcept
    {
        return cancel != nullptr && cancel->load(std::memory_order_acquire);
    }
};

// A handler fills `out` and returns its status. It runs on the calling thread under the service's lock, so it must
// not call back into the same service.
using DiagHandler = DiagStatus (*)(void* context, const DiagCall& call, DiagSnapshot& out);

// A command's declaration. The name, owner and summary must have static storage.
struct DiagCommandSpec
{
    cont::StringView name;
    cont::StringView owner;   // the registering module ("perf", "ceir", ...)
    cont::StringView summary; // one line for listings
    DiagAuthority    authority  = DiagAuthority::Read;
    bool             takes_path = false;
};

struct DiagServiceConfig
{
    cont::StringView root;                                           // file root for path arguments; empty = none
    crd::u64         max_bundle_bytes  = 256ULL * 1024ULL * 1024ULL; // bundle.inspect refuses a larger file unread
    crd::u64         max_capture_bytes = 256ULL * 1024ULL * 1024ULL; // capture.stop refuses a larger capture unwritten
    crd::u32         worker_timeout_ms = 20U;                        // jobs.waits' bound on the worker poll
};

// The command service. Built-in commands, in listing order:
//   diag.commands      Read    every registered command with its owner, authority and path argument
//   diag.capabilities  Read    the doctor's modes and dependencies, the command schema and the grant
//   jobs.waits         Read    parked fibers and their wait edges, and each worker's responsiveness
//   memory.allocators  Read    every allocator registered with the profiler and its live statistics
//   bundle.inspect     Read    a post-mortem bundle under the root, through the bounded non-executing importer
//   capture.start      Record  open a capture window (the profiler's sample rings are cleared)
//   capture.stop       Record  write the open window as a CPROF capture file under the root (never overwriting)
// Calls are serialized; handlers run on the calling thread.
class DiagCommandService
{
public:
    DiagCommandService(DiagAuthoritySet grant, const DiagServiceConfig& config,
                       crd::memory::IAllocator* alloc = crd::memory::default_allocator());
    ~DiagCommandService() = default;

    DiagCommandService(const DiagCommandService&)            = delete;
    DiagCommandService& operator=(const DiagCommandService&) = delete;
    DiagCommandService(DiagCommandService&&)                 = delete;
    DiagCommandService& operator=(DiagCommandService&&)      = delete;

    // Register a command from an upper module. Returns false for a duplicate or malformed name, a None authority, a
    // null handler or a full table.
    [[nodiscard]] bool register_command(const DiagCommandSpec& spec, DiagHandler handler, void* context);

    [[nodiscard]] DiagResult execute(const DiagRequest& request, const std::atomic<bool>* cancel = nullptr);

    [[nodiscard]] DiagAuthoritySet       grant() const noexcept { return m_grant; }
    [[nodiscard]] crd::u32               command_count() const noexcept;
    [[nodiscard]] const DiagCommandSpec* command_at(crd::u32 index) const noexcept;

    // Evidence that refusals happen before work: handlers run, and bytes the built-in file readers read.
    [[nodiscard]] crd::u64 handler_runs() const noexcept;
    [[nodiscard]] crd::u64 file_bytes_read() const noexcept;
    // Capture windows opened so far, and whether one is open.
    [[nodiscard]] crd::u64 capture_window() const noexcept;
    [[nodiscard]] bool     capture_open() const noexcept;

private:
    struct Entry
    {
        DiagCommandSpec spec;
        DiagHandler     handler = nullptr;
        void*           context = nullptr;
    };

    static DiagStatus run_commands(void* self, const DiagCall& call, DiagSnapshot& out);
    static DiagStatus run_capabilities(void* self, const DiagCall& call, DiagSnapshot& out);
    static DiagStatus run_jobs_waits(void* self, const DiagCall& call, DiagSnapshot& out);
    static DiagStatus run_allocators(void* self, const DiagCall& call, DiagSnapshot& out);
    static DiagStatus run_bundle_inspect(void* self, const DiagCall& call, DiagSnapshot& out);
    static DiagStatus run_capture_start(void* self, const DiagCall& call, DiagSnapshot& out);
    static DiagStatus run_capture_stop(void* self, const DiagCall& call, DiagSnapshot& out);

    static constexpr crd::u32 kNone = 0xFFFFFFFFU;

    [[nodiscard]] crd::u32 find(cont::StringView name) const noexcept;
    void refuse(DiagResult& result, const DiagRequest& request, DiagStatus status, cont::StringView reason) const;
    void page(DiagResult& result, const DiagRequest& request, crd::u64 offset) const;

    crd::memory::IAllocator* m_alloc;
    DiagAuthoritySet         m_grant;
    cont::String             m_root;
    DiagServiceConfig        m_config;
    cont::Array<Entry>       m_commands;

    mutable std::mutex m_mutex;
    DiagSnapshot       m_snapshot;            // the retained snapshot pages are cut from
    crd::u32           m_snapshot_of = kNone; // its command's index (kNone: none retained)
    crd::u64           m_generation  = 0U;
    crd::u64           m_handler_runs    = 0U;
    crd::u64           m_file_bytes_read = 0U;
    crd::u64           m_capture_window  = 0U;
    bool               m_capture_open    = false;
};

} // namespace crd::perf
