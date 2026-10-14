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
// - os_thread_alive: is the thread an identity names still running in this process? An identity is read from inside
//   the thread (os_current_thread_identity) and is the OS id plus the thread's start time, so an id the OS has handed
//   to a later thread is not mistaken for the recorded one. Windows opens the id (OpenThread), requires it to belong
//   to this process, compares its creation time (GetThreadTimes) and asks whether it has exited; Linux reads
//   /proc/self/task/<tid>/stat, compares its start time and treats a zombie, or a task whose kernel flags carry
//   PF_EXITING, as ended: that flag is set before a join of the thread can return, while the task stays listed as
//   running until the kernel finishes its exit. A count of the process's threads is no oracle: the OS loader and
//   thread pool start and retire threads of their own at any time.
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

// A thread as the OS names it.
struct OsThreadIdentity
{
    crd::u64 id    = 0U;    // Windows: the thread id; Linux: the kernel task id (gettid)
    crd::u64 start = 0U;    // Windows: creation time (FILETIME ticks); Linux: starttime (clock ticks since boot)
    bool     known = false; // false when the OS could not be asked; such an identity is never judged
};

// The calling thread's identity.
[[nodiscard]] OsThreadIdentity os_current_thread_identity() noexcept;

// Present while the thread `t` names runs in this process; Absent once it has ended, or when its id now names another
// thread or no thread of this process; Unavailable when `t` is not known or the OS cannot be asked.
[[nodiscard]] OsProbeResult os_thread_alive(const OsThreadIdentity& t) noexcept;

} // namespace crd
