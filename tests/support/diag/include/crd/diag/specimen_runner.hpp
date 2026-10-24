#pragma once

// crd-diag-harness -- the DIAG.0 specimen runner.
//
// The fixture the rest of DIAG inherits. Its single job is to distinguish an
// *expected detection* (a specimen behaved exactly as a detector should make it
// behave) from an *instrument failure* (the tool, not the code under test, is the
// thing that broke): a missing executable, a timeout, an absent sanitizer, a
// mismatched binary, a denied output path or a zero-specimen selection must never
// be reported as a pass. Every dangerous specimen runs in an isolated child
// process with a mandatory timeout bound; the parent classifies the child's exit,
// signal, echoed identity and sanitizer tag into one Verdict.
//
// Contract: docs/design/runtime-diagnostics.md (shared acceptance rules); owner
// ROADMAP DIAG.0. No intentional undefined behaviour runs in this process -- it
// only ever spawns children. Engine containers only (docs/CODING.md): no owning
// STL containers anywhere, including this test-support code.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::diag
{

// Why the harness reached its verdict. Clean / Crashed / SanitizerCaught are the
// three *positive* outcomes (a specimen did exactly what a working detector makes
// it do). The rest are *instrument failures* the harness must name rather than
// silently pass, plus Unexpected for "ran, but not what was asked" (also a
// failure, never a pass).
enum class Verdict : crd::u8
{
    Clean,             // exited 0, identity matched: the expected-clean positive
    Crashed,           // died by signal / abnormal exit: the expected-crash positive
    SanitizerCaught,   // the specimen died and its output carries its sanitizer's report: the sanitizer positive
    InstrumentAbsent,  // a sanitizer catch was expected but the specimen carries no sanitizer
    MissingSymbolizer, // a sanitizer report whose frames are not symbolized, so it cannot name the expected site
    ZeroSelection,     // the filter selected no specimen at all
    DeniedOutput,      // the requested output path is not writable
    MismatchedBinary,  // the specimen's stamped identity != the expected identity
    MissingExecutable, // the path does not exist or the child could not be spawned
    Timeout,           // the child exceeded its time bound and was killed
    Unexpected,        // the child ran but its outcome did not match the expectation
};

const char* verdict_name(Verdict v);

// What the caller expects of a specimen. The harness's verdict is judged against
// this: e.g. Want::SanitizerCatch on a specimen tagged "none" yields
// InstrumentAbsent, on a sanitized specimen that dies printing its sanitizer's
// report yields SanitizerCaught, and on a sanitized specimen that exits 0, or dies
// without that report in its output, yields Unexpected (a real instrument
// failure: the overflow was not caught, or the report was lost). An exit status
// alone is never a detection (DIAG.10b).
struct Expectation
{
    enum class Want : crd::u8
    {
        CleanExit,      // exit 0, no crash
        Crash,          // die by signal / abnormal termination
        SanitizerCatch, // abort with a sanitizer diagnostic
    };

    Want                     want = Want::CleanExit;
    crd::containers::String  expected_identity; // empty: skip the identity check
    crd::containers::String  output_dir;        // non-empty: probe writability before spawning; a file -> DeniedOutput
    crd::containers::String  expected_site;     // non-empty: a sanitizer catch's report must name this function
    crd::u32                 timeout_ms = 10000U;
};

// The child's combined output kept for evidence (a sanitizer report is a few KiB; the cap bounds a runaway child).
inline constexpr crd::usize kMaxOutputBytes = 1024U * 1024U;

// The classified result of one run. Never carries an exception; a spawn failure is
// MissingExecutable, not a throw. host_tuple records OS/ISA/compiler per the shared
// acceptance rules.
struct Outcome
{
    Verdict                 verdict = Verdict::Unexpected;
    crd::i32                exit_code = 0;
    bool                    crashed = false;
    bool                    timed_out = false;
    bool                    memory_bound_enforced = false; // false on sanitizer children by design (RLIMIT_AS breaks ASan)
    crd::containers::String identity;   // as echoed by the specimen on stdout
    crd::containers::String sanitizer;  // "asan" | "tsan" | "ubsan" | "none", as echoed by the specimen
    crd::containers::String reason;     // human-readable detail
    crd::containers::String host_tuple; // os/isa/compiler recorded on every run
    crd::containers::String output;     // the child's stdout and stderr together, first kMaxOutputBytes kept
    bool                    output_truncated = false; // the child wrote more than kMaxOutputBytes
};

// What the build lane promises, from its configuration (the CMake options compiled into this harness), never from a
// specimen's or a test's own compile flags. A lane that loses a sanitizer -- a specimen or a test built without it --
// must fail its specimen tests rather than see every specimen report InstrumentAbsent and pass (DIAG.10b). Tests
// judge a specimen against this promise and check that their own binary carries it.
struct LanePromise
{
    bool asan = false;             // CRD_ENABLE_ASAN for the configuration being built
    bool tsan = false;             // CRD_ENABLE_TSAN
    bool ubsan = false;            // CRD_ENABLE_UBSAN
    bool leak_route = false;       // LeakSanitizer: ASan built by gcc or clang on Linux
    bool use_after_return = false; // fake-stack use-after-return reports: ASan built by gcc or clang
};

LanePromise lane_promise();

// Run one specimen executable as a bounded child process and classify it against
// `expect`. Pre-spawn instrument failures (denied output, missing executable) are
// detected before any child starts. Never throws.
Outcome run_specimen(const crd::containers::String& exe_path,
                     const crd::containers::Array<crd::containers::String>& args,
                     const Expectation& expect);

// Select from `candidates` those whose path contains `filter` (empty filter = all),
// then run the first match. An empty selection is the ZeroSelection instrument
// failure -- the harness fails rather than reporting a vacuous pass.
Outcome select_and_run(const crd::containers::Array<crd::containers::String>& candidates,
                       const crd::containers::String& filter,
                       const Expectation& expect);

// OS/ISA/compiler tuple recorded on every run.
crd::containers::String host_tuple();

} // namespace crd::diag
