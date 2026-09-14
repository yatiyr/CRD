#pragma once

// crd-perf -- lifecycle-safe diagnostic recording + bounded emergency record (DIAG.2b).
//
// A preallocated bounded ring of DiagnosticEvents (no allocation on the record path once
// initialised), reader registration that hands back a lifetime token and retires slots safely on
// deregistration (a stale token can never retire a reused slot -- generation-checked), and an
// INDEPENDENT minimal emergency record captured without the ring's mutex, so a crash taken while
// the ordinary log/ring lock is held still yields a record. Reentrancy and early-init /
// late-shutdown are defined: every entry point is a no-op (not a fault) before init and after
// shutdown. DG08's unsynchronised registry reads are addressed by taking the same lock for reads
// and writes here rather than assuming a writer mutex protects readers.
//
// Async-signal-safe capture from a real signal handler is DIAG.5b's job; this slice makes the
// record independent of the ordinary lock and bounded, and proves it under concurrency.
//
// Contract: docs/design/runtime-diagnostics.md#diag-2b; ADR-0133; DG08/DG10.

#include <crd/perf/diagnostics.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{

// A lifetime token for a registered reader. Invalid tokens (default-constructed, or from a slot
// that was since retired and reused) are rejected by deregister_reader.
struct ReaderToken
{
    crd::u32 slot       = 0xFFFFFFFFU;
    crd::u32 generation = 0U;

    [[nodiscard]] bool valid() const noexcept { return slot != 0xFFFFFFFFU; }
};

// The independent, fixed-size emergency record. No owned storage -- fixed char buffers so it can
// be written without allocation and read after the recorder is gone.
struct EmergencyRecord
{
    bool      present      = false;
    EventCode code         = 0U;
    Severity  severity     = Severity::Info;
    crd::u64  timestamp_ns = 0U;
    crd::u32  line         = 0U;
    char      file[128]    = {};
    char      message[192] = {};
};

class DiagnosticRecorder
{
public:
    DiagnosticRecorder() = default;
    ~DiagnosticRecorder() { shutdown(); }

    DiagnosticRecorder(const DiagnosticRecorder&)            = delete;
    DiagnosticRecorder& operator=(const DiagnosticRecorder&) = delete;

    // Preallocate the ring to `capacity` events. Idempotent-safe: re-init after shutdown is legal.
    bool init(crd::u32 capacity) noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool is_initialized() const noexcept;

    // Record into the ring, overwriting the oldest when full. No-op before init / after shutdown
    // (so late producers during shutdown do not fault). Safe from many threads.
    void record(const DiagnosticEvent& e);

    // Copy the live records (oldest first) into `out`. Cleared first. No-op before init.
    void snapshot(cont::Array<DiagnosticEvent>& out) const;

    [[nodiscard]] crd::u32 count() const noexcept;          // live records, <= capacity
    [[nodiscard]] crd::u64 total_recorded() const noexcept; // monotonic; count() + dropped-to-overwrite
    [[nodiscard]] crd::u32 capacity() const noexcept;

    // Reader registration. Returns an invalid token when the (bounded) reader table is full.
    [[nodiscard]] ReaderToken register_reader() noexcept;
    bool                      deregister_reader(ReaderToken token) noexcept;
    [[nodiscard]] crd::u32    reader_count() const noexcept;

    // Emergency capture -- writes the minimal record WITHOUT taking the ring mutex, so it succeeds
    // even if a thread is mid-record() (or the log mutex is held) when a fatal path runs.
    void                       capture_emergency(const DiagnosticEvent& e) noexcept;
    [[nodiscard]] EmergencyRecord emergency() const noexcept;

private:
    struct Impl;
    Impl* m_impl = nullptr; // pimpl keeps std::mutex out of the public header
};

} // namespace crd::perf
