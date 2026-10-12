#pragma once

// What every route of the diagnostic application shares (DIAG.11a): the headless setup, the build line, the
// application's event codes, starting the engine's DiagnosticHost from an authored profile, running one diagnostic
// command through the host's own service, and waiting for the host to write a capture.
//
// Every line the application prints starts with CONSUMER_ and carries key=value fields, so
// scripts/test-package-consumer.py can judge it.

#include <crd/core/types.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/diagnostics.hpp>

namespace diag_consumer
{

namespace cp = crd::perf;

// The application's own event codes, above the range the engine's foundation owns (registered at start_host).
inline constexpr cp::EventCode kCodeRangeFirst = 0x00A10000U;
inline constexpr cp::EventCode kCodeRangeLast  = 0x00A1FFFFU;
inline constexpr cp::EventCode kOrderRecorded  = 0x00A10001U; // the deliberate failure's bookings (main.cpp)
inline constexpr cp::EventCode kTotalNegative  = 0x00A10002U; // the deliberate failure's invariant (main.cpp)
inline constexpr cp::EventCode kOrderReserved  = 0x00A10010U; // allocator example: a = order, b = bytes
inline constexpr cp::EventCode kOrderReleased  = 0x00A10011U; // allocator example: a = order
inline constexpr cp::EventCode kHeapViolation  = 0x00A10012U; // allocator example: a = ViolationKind, b = byte offset
inline constexpr cp::EventCode kSliceSummed    = 0x00A10020U; // jobs example: a = slice, b = its sum
inline constexpr cp::EventCode kLimitsLoaded   = 0x00A10030U; // reload example: a = generation, b = max_order
inline constexpr cp::EventCode kLimitsKept     = 0x00A10031U; // reload example: a = generation kept
inline constexpr cp::EventCode kFrameWork      = 0x00A10040U; // capture example: a = frame, b = items

// A failing check must end the process with its evidence, never wait on a dialog nobody will click.
void run_headless();

// CONSUMER_BUILD: what this binary was compiled with.
void print_build();

// Load the profile at `profile_path` and start `host` from it, printing CONSUMER_HOST_STARTED with the modes it runs
// (or CONSUMER_PROFILE_REFUSED / CONSUMER_HOST_REFUSED). Registers the application's event codes and its replay
// recipe. False when the host did not start.
[[nodiscard]] bool start_host(cp::DiagnosticHost& host, const char* profile_path, const char* recipe);

// Run `command` (with at most one named argument) on `service` and print CONSUMER_COMMAND with its status and JSON.
cp::DiagStatus run_command(cp::DiagCommandService& service, const char* command, const char* arg_name = nullptr,
                           const char* arg_value = nullptr);

// Poll `host` until it has written `count` bundles in all, or `timeout_ms` elapsed. True when it has.
[[nodiscard]] bool wait_for_bundles(cp::DiagnosticHost& host, crd::u64 count, crd::u32 timeout_ms);

// CONSUMER_BUNDLE: the file name of the last bundle the host wrote, with `label` saying which capture it is.
void print_last_bundle(const cp::DiagnosticHost& host, const char* label);

} // namespace diag_consumer
