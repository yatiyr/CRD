#pragma once

// crd-perf -- typed diagnostic events, identities and authorable policy (DIAG.2a).
//
// The shared schema every diagnostic producer emits: a versioned, severity-tagged event with
// a source/symbol identity, a monotonic timestamp and a stable generation key, plus an
// authorable bounded policy with last-known-good reload. C++ producers and (through the code
// bridge) CEIR consumers emit the SAME structured context to CLI/JSON and to logging, without
// foundation depending upward on CEIR.
//
// Contract: docs/design/runtime-diagnostics.md#diag-2a; ADR-0133; DG13/DG18.

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{
namespace cont = crd::containers;

// Wire-format version. Producers stamp it; consumers reject unknown versions explicitly.
inline constexpr crd::u32 kDiagnosticSchemaVersion = 1U;

// The severity/fatal taxonomy. Recoverable errors, developer assertions, instrument failures
// and fatal invariant violations are distinct classes -- not a single "error" bucket -- so a
// consumer can route each correctly. A fatal invariant is required runtime validation and must
// not be compiled out with debug asserts (see check_invariant / CRD_DIAG_INVARIANT below).
enum class Severity : crd::u8
{
    Info,               // informational; never actionable on its own
    Warning,            // anomalous but handled
    RecoverableError,   // an operation failed; the caller has a recovery path
    DeveloperAssertion, // a programming-error check (may be debug-only at the call site)
    InstrumentFailure,  // a diagnostic instrument itself failed (must never mask real work)
    FatalInvariant      // an invariant that must hold in every build was violated
};

[[nodiscard]] constexpr bool is_fatal(Severity s) noexcept { return s == Severity::FatalInvariant; }
[[nodiscard]] cont::StringView severity_name(Severity s) noexcept;

// A typed event code. u32 with documented, non-overlapping reserved ranges so producers in
// different modules share one identity space: foundation owns [0, kFoundationCodeMax]; other
// modules (CEIR, renderer, ...) register a range above it from their own side, so foundation
// never learns those modules exist. See register_code_range / owning_module.
using EventCode = crd::u32;
inline constexpr EventCode kFoundationCodeMax = 0x0000FFFFU;

// Where an event was produced. The strings are borrowed (compile-time literals via the macro),
// not owned -- copying an event copies only the views.
struct SourceIdentity
{
    cont::StringView file;
    cont::StringView symbol;
    crd::u32         line = 0U;
};

// A stable, monotonic key identifying a logical generation (e.g. an asset reload, a policy
// epoch). Wrap is explicit: next_generation() flags overflow rather than silently reusing 0.
struct GenerationKey
{
    crd::u64 value = 0U;
};

// One diagnostic event. `message` is owned (survives the producer's frame); `message_truncated`
// is set when the message was clipped to the active policy's byte bound -- truncation is never
// silent.
struct DiagnosticEvent
{
    crd::u32       schema_version = kDiagnosticSchemaVersion;
    EventCode      code           = 0U;
    Severity       severity       = Severity::Info;
    SourceIdentity source;
    GenerationKey  generation;
    crd::u64       timestamp_ns   = 0U; // monotonic clock (see diagnostic_now_ns)
    cont::String   message;
    bool           message_truncated = false;
};

// Monotonic nanosecond clock for event timestamps (steady, never wall-clock).
[[nodiscard]] crd::u64 diagnostic_now_ns() noexcept;

// ---------------------------------------------------------------------------
// Authorable bounded policy (DG18)
// ---------------------------------------------------------------------------

// Hard bounds a policy may never exceed; a candidate outside them is rejected.
inline constexpr crd::u32 kMaxPolicyEvents       = 1U << 20; // 1,048,576
inline constexpr crd::u32 kMaxPolicyMessageBytes = 64U * 1024U;

// The authorable policy. `min_severity` gates recording; the two bounds cap storage and message
// size so a hostile or buggy policy cannot demand unbounded resources.
struct DiagnosticPolicy
{
    crd::u32 schema_version    = kDiagnosticSchemaVersion;
    Severity min_severity      = Severity::Warning;
    crd::u32 max_events        = 4096U;
    crd::u32 max_message_bytes = 512U;
};

// Why a policy load did or did not take effect. Every rejection reason is explicit.
enum class PolicyLoadStatus : crd::u8
{
    Committed,               // validated; became the active policy at a new generation
    RejectedUnknownVersion,  // schema_version not understood
    RejectedOutOfBounds,     // a field exceeds a hard bound (or is zero where non-zero required)
    RejectedGenerationWrap   // the generation counter would wrap; active policy retained
};

// Holds the active policy behind a validating, last-known-good reload. A rejected candidate
// leaves both the active policy AND the generation untouched -- malformed input can never move
// the active generation. Not internally synchronised; DIAG.2b owns lifecycle/concurrency.
class DiagnosticPolicyStore
{
public:
    DiagnosticPolicyStore() = default;

    // Validate `candidate` and, only on success, commit it and advance the generation.
    PolicyLoadStatus load(const DiagnosticPolicy& candidate) noexcept;

    [[nodiscard]] const DiagnosticPolicy& active() const noexcept { return m_active; }
    [[nodiscard]] crd::u32 generation() const noexcept { return m_generation; }
    [[nodiscard]] bool generation_overflowed() const noexcept { return m_overflowed; }

private:
    DiagnosticPolicy m_active{};        // default policy is valid and active from construction
    crd::u32         m_generation = 1U; // 0 is reserved for "never loaded"
    bool             m_overflowed = false;
};

// Does the policy admit an event of this severity? Fatal invariants are always admitted
// regardless of min_severity -- required validation is never policy-gated away.
[[nodiscard]] bool policy_admits(const DiagnosticPolicy& p, Severity s) noexcept;

// Build an event, clipping `message` to the active policy's max_message_bytes and setting
// message_truncated when it did. Stamps schema version, monotonic timestamp and generation.
[[nodiscard]] DiagnosticEvent make_event(EventCode code, Severity sev, SourceIdentity src,
                                         cont::StringView message,
                                         const DiagnosticPolicyStore& policy,
                                         GenerationKey gen = {});

// ---------------------------------------------------------------------------
// Serialization -- the shared structured context (CLI/JSON + logging)
// ---------------------------------------------------------------------------

// Append the event as a single-line JSON object to `out`. Deterministic key order so C++ and
// CEIR emit byte-identical context for the same event.
void to_json(const DiagnosticEvent& e, cont::String& out);

// Append a human-readable log line to `out`.
void to_log_line(const DiagnosticEvent& e, cont::String& out);

// ---------------------------------------------------------------------------
// Cross-module code bridge (no upward dependency)
// ---------------------------------------------------------------------------

// Register a module's reserved EventCode range [lo, hi]. Fails (returns false) if the range
// dips into the foundation range, is inverted, or overlaps an existing registration. CEIR calls
// this from its own translation unit, so crd-perf never includes a CEIR header.
bool register_code_range(cont::StringView module_name, EventCode lo, EventCode hi) noexcept;

// The module that owns `code` ("foundation" for [0, kFoundationCodeMax], a registered name
// above it, or "unassigned").
[[nodiscard]] cont::StringView owning_module(EventCode code) noexcept;

// Test/reset hook: clear all non-foundation registrations.
void reset_code_registry() noexcept;

// ---------------------------------------------------------------------------
// Fatal-invariant validation -- ALWAYS active, never compiled out with asserts
// ---------------------------------------------------------------------------

// Invoked when a fatal invariant fails. Default terminates the process (after reporting).
// Tests may install a non-terminating handler to observe the emitted event.
using FatalInvariantHandler = void (*)(const DiagnosticEvent& e);
void set_fatal_invariant_handler(FatalInvariantHandler handler) noexcept;

// Required runtime validation: if `cond` is false, build a FatalInvariant event and route it to
// the fatal handler. Unlike CRD_ASSERT this is present in every build. Prefer the macro, which
// captures the source identity.
void check_invariant(bool cond, EventCode code, SourceIdentity src, cont::StringView message);

#define CRD_DIAG_INVARIANT(cond, code, msg)                                                                            \
    ::crd::perf::check_invariant((cond), (code),                                                                       \
                                 ::crd::perf::SourceIdentity{cont::StringView{__FILE__},                               \
                                                             cont::StringView{static_cast<const char*>(__func__)},    \
                                                             static_cast<crd::u32>(__LINE__)},                         \
                                 (msg))

} // namespace crd::perf
