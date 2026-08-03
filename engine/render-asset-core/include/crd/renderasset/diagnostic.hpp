#pragma once

// crd-render-asset-core — structured render-asset diagnostics (RAF-1, mission §15).
//
// One canonical diagnostic model for the ENTIRE render-asset pipeline: every
// cooker, loader, and validator reports through `Diagnostic` + `DiagnosticList`
// instead of a bool or a bespoke error string. A diagnostic is machine-readable
// (a stable `DiagCode`) AND human-readable (a message plus the offending asset /
// field / expected / actual / capability), so the same record drives a test
// assertion, a build-log line, and a hot-reload failure report (RAF-11).
//
// ⛔ no std containers: the message/field strings are crd::containers::String and
// the list is crd::containers::Array, both allocator-owned by the DiagnosticList.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::renderasset
{
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;

enum class Severity : u8
{
    Info,
    Warning,
    Error,
};

// Stable diagnostic codes. IMPORTANT: a code's IDENTITY is its NAME string
// (via `diag_code_name`), never its ordinal — reorder freely, cooked/reported
// diagnostics reference the name, so this enum is NOT append-only-fragile.
enum class DiagCode : u8
{
    Ok,
    MalformedPath,         // no "://", or empty after the scheme
    UnknownScheme,         // scheme is not engine/app/plugin/test/crd
    PathEscapesRoot,       // a ".." segment popped above the mount root
    EmptyPath,             // scheme present but nothing after "://"
    EmptySegment,          // (reserved) a segment collapsed to empty in a strict context
    IdCollision,           // two DISTINCT canonical paths hash to the same AssetId
    DuplicateRegistration, // same id re-registered with a DIFFERENT canonical path
    MissingDependency,     // a referenced dependency id is not registered
    CyclicDependency,      // the dependency graph is not a DAG
    MalformedBlob,           // a cooked blob's magic/structure is wrong
    TruncatedBlob,           // a cooked blob is shorter than its header/deps require
    SchemaMismatch,          // a cooked blob's schema version differs from what the loader expects
    TypeMismatch,            // a cooked blob's asset type differs from what the loader expects
    DuplicateStage,          // a program has two modules of the same shader stage
    IllegalStageComposition, // a program's stage set is not a legal pipeline
    StageIoMismatch,         // a downstream stage input has no matching upstream output
    AttachmentMismatch,      // fragment outputs do not match the render-target signature
    BindingConflict,         // one binding name declared with conflicting kind/frequency
    MaterialLightingAccess,  // a material declared a LIGHTING channel (materials are surface-only)
    InvalidOverride,         // a material instance overrides a param that does not exist / type-mismatches
    MissingResource,         // a required texture/resource param is neither defaulted nor bound
    IncompatibleSurface,     // a technique consumes a surface channel the material does not produce
    UnsupportedPhase,        // a technique was asked to run in a render phase it does not support
    DuplicateExecutor,       // a pass-executor id was registered twice
    UnknownExecutor,         // a pass payload references an unregistered executor id
    InvalidParam,            // a pass payload param is missing / unknown / type-mismatched vs the executor schema
    InvalidSlot,             // a pass payload resource slot is missing / kind-or-access-mismatched vs the schema
    QueueMismatch,           // a pass payload's queue differs from the executor's declared queue
};

// The name IS the stable identity of a code (see enum comment).
[[nodiscard]] StringView diag_code_name(DiagCode code) noexcept;
[[nodiscard]] StringView severity_name(Severity sev) noexcept;

// A single structured diagnostic. Aggregate on purpose so DiagnosticList can
// brace-init each String with its own allocator (no hidden default_allocator).
// Empty String fields mean "not applicable to this diagnostic".
struct Diagnostic
{
    Severity severity = Severity::Error;
    DiagCode code = DiagCode::Ok;
    String message;    // human text
    String asset;      // canonical (or raw) asset the diagnostic is about
    String field;      // offending field / path segment
    String expected;   // expected value / shape
    String actual;     // actual value / shape
    String capability; // required device capability (for capability diagnostics)
};

// An allocator-owning collection of diagnostics. All member Strings of every
// Diagnostic are built from THIS list's allocator, so the list owns its text.
class DiagnosticList
{
public:
    explicit DiagnosticList(memory::IAllocator* alloc) noexcept : m_items(alloc), m_alloc(alloc) {}

    // Full emit. Optional context fields default to empty (== not applicable).
    void emit(Severity severity, DiagCode code, StringView message, StringView asset = {}, StringView field = {},
              StringView expected = {}, StringView actual = {}, StringView capability = {});

    void error(DiagCode code, StringView message, StringView asset = {}, StringView field = {})
    {
        emit(Severity::Error, code, message, asset, field);
    }
    void warn(DiagCode code, StringView message, StringView asset = {}, StringView field = {})
    {
        emit(Severity::Warning, code, message, asset, field);
    }
    void info(DiagCode code, StringView message, StringView asset = {}, StringView field = {})
    {
        emit(Severity::Info, code, message, asset, field);
    }

    [[nodiscard]] usize size() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.size() == 0; }
    [[nodiscard]] const Diagnostic& operator[](usize i) const noexcept { return m_items[i]; }

    [[nodiscard]] bool has_errors() const noexcept;
    // Count of diagnostics carrying a given code (for precise gating).
    [[nodiscard]] usize count(DiagCode code) const noexcept;
    // True if ANY diagnostic carries `code` (short-circuits on the first match).
    [[nodiscard]] bool contains(DiagCode code) const noexcept;

    [[nodiscard]] memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    Array<Diagnostic> m_items;
    memory::IAllocator* m_alloc;
};
} // namespace crd::renderasset
