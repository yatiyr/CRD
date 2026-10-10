#pragma once

// crd-perf -- the flight recorder's host side and its flight.* diagnostic commands (DIAG.9b).
//
// A FlightCommandHost is the sink a host installs on its FlightRecorder: it keeps a copy of the last capture the
// recorder delivered (poll() on the host's frame loop, finish() at shutdown or on a fatal path) and passes each one on
// to an optional forward sink (a host that writes every capture to disk). Once a sink is set the recorder hands every
// capture to it and take_capture() finds none, so the host is where a delivered capture can still be read and written.
//
// The flight.* commands, owner "perf", read and drive it through the command service:
//   flight.status   Read    the recorder's state and active policy, and the retained capture: its trigger, windows,
//                           loss and replay as the summary and its events as items
//   flight.trigger  Record  fire a manual trigger (optional argument message=<text>)
//   flight.policy   Record  load the flight-policy asset at the request path (crd/perf/diag_assets.hpp) and reload
//                           the recorder's policy; a refused document or a refused reload keeps the active policy
//   flight.write    Record  write the retained capture as a flight bundle at the request path (never overwriting),
//                           with this build's identity and the host's refreshed module snapshot
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/perf/diag_commands.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/core/types.hpp>

#include <atomic>
#include <mutex>

namespace crd::perf
{

class FlightCommandHost
{
public:
    explicit FlightCommandHost(FlightRecorder& recorder) noexcept;
    // Removes itself as the recorder's sink when it is still the installed one.
    ~FlightCommandHost();

    FlightCommandHost(const FlightCommandHost&)            = delete;
    FlightCommandHost& operator=(const FlightCommandHost&) = delete;
    FlightCommandHost(FlightCommandHost&&)                 = delete;
    FlightCommandHost& operator=(FlightCommandHost&&)      = delete;

    // Install this host as the recorder's sink.
    void install_sink() noexcept;

    // Called with each capture after it is retained (nullptr: none). Set it before captures can arrive.
    void set_forward(FlightCaptureSink sink, void* user) noexcept;

    // The recorder sink: retains a copy of `capture` (replacing the previous one), then forwards it.
    static void deliver(const FlightCapture& capture, void* host);

    // Copy the retained capture into `out`. False when none has been delivered.
    [[nodiscard]] bool retained(FlightCapture& out) const;

    // Captures delivered to this host so far.
    [[nodiscard]] crd::u64 received() const noexcept { return m_received.load(std::memory_order_acquire); }

    [[nodiscard]] FlightRecorder& recorder() noexcept { return m_recorder; }

    // Enumerate the loaded modules now (enumerate_loaded_modules) and merge them into the host's snapshot
    // (merge_module_snapshot): a module no longer loaded stays, marked unloaded, so a bundle still names the binary a
    // capture's events came from. Call it when the host starts and after it loads or unloads modules; flight.write
    // calls it before writing. It takes the loader lock: never call it from a fatal path or a forward sink. Returns
    // false when this platform could not enumerate (the snapshot is unchanged).
    bool refresh_modules();

    // A copy of the module snapshot (empty before the first refresh), for a forward sink that writes its own bundle.
    [[nodiscard]] cont::Array<ModuleIdentity> modules() const;
    // True when a module was dropped because the snapshot reached kMaxLiveModules.
    [[nodiscard]] bool modules_overflow() const;

private:
    FlightRecorder&                m_recorder;
    mutable std::mutex             m_mutex; // the retained capture
    FlightCapture                  m_last;
    bool                           m_has_last = false;
    mutable std::mutex             m_modules_mutex; // the module snapshot
    cont::Array<ModuleIdentity>    m_modules;
    bool                           m_modules_overflow = false;
    std::atomic<crd::u64>          m_received{0U};
    std::atomic<bool>              m_installed{false};
    std::atomic<FlightCaptureSink> m_forward{nullptr};
    std::atomic<void*>             m_forward_user{nullptr};
};

// Register flight.status, flight.trigger, flight.policy and flight.write. Returns false when any of them could not be
// registered. `host` must outlive the service.
[[nodiscard]] bool register_flight_commands(DiagCommandService& service, FlightCommandHost& host);

} // namespace crd::perf
