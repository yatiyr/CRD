#pragma once

// The operating system's own view of what this process holds: the independent oracle of the lifetime ledger
// (crd/core/lifetime_ledger.hpp, DIAG.10c).
//
// The ledger counts what the engine says it acquired and released; these probes ask the OS whether the thing is
// really held, so a test can judge a wired site by something it does not share code with.
//
// - os_file_open_in_process: does any handle or descriptor of THIS process refer to the file at `path`?
//   Windows walks the process's own handle table (NtQueryInformationProcess, ProcessHandleSnapshotInformation) and
//   compares each disk-file handle's normalized final path with the path's; handles of other processes (a virus
//   scanner, an indexer) never count. Linux compares the (device, inode) of every descriptor in /proc/self/fd with the
//   path's. The probe's own handle on Windows is excluded.
// - os_range_mapped: how much of [base, base + bytes) is reserved or mapped. Windows walks VirtualQuery (any state
//   but MEM_FREE); Linux reads /proc/self/maps.
//
// Both are diagnostics for tests and investigations, not hot paths: they make system calls and read the process
// tables, allocate no heap memory (the handle table is read into pages from the OS), and judge one moment.
// Elsewhere (another OS, a path that cannot be opened to identify it) the answer is Unavailable with a reason, never
// Absent.

#include <crd/core/types.hpp>

namespace crd
{

// NOLINTNEXTLINE(performance-enum-size)
enum class OsPresence : crd::u8
{
    Present,     // held (a file) or wholly reserved or mapped (a range)
    Absent,      // not held, or no byte of the range is reserved or mapped
    Partial,     // part of the range is reserved or mapped (ranges only)
    Unavailable, // the OS could not be asked; `reason` says why
};
[[nodiscard]] const char* os_presence_name(OsPresence p) noexcept;

struct OsProbeResult
{
    OsPresence  presence = OsPresence::Unavailable;
    const char* reason   = ""; // a static string, set when presence is Unavailable
};

[[nodiscard]] OsProbeResult os_file_open_in_process(const char* path) noexcept;
[[nodiscard]] OsProbeResult os_range_mapped(const void* base, crd::usize bytes) noexcept;

} // namespace crd
