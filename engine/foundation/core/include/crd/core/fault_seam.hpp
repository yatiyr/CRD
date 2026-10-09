#pragma once

// crd-core -- the process-wide I/O fault seam (DIAG.9b).
//
// The file reads every engine module shares (crd-platform's read_file_binary, read_file_range and AsyncFile's
// completion jobs) consult this seam before they touch the file. With no hook installed the consult is one atomic
// load and the read proceeds. A diagnostics host installs one hook (crd-perf's seeded FaultInjector provides
// fault_io_hook) to fail a read, cut it short, delay its completion or cancel it, so a test can drive the real
// recovery paths of load, reload and teardown without a broken disk.
//
// The seam is a build capability. A shipping build (CRD_SHIPPING) compiles it out: installing is refused and the
// consult always answers Proceed, so no request, authored asset or host can inject a fault there.
//
// One hook at a time. The hook runs under the seam's mutex, so uninstall_io_fault_hook() returns only after every
// call in progress has finished; the hook's user state may be destroyed after that. The hook must not read a file
// through the seam, and must not block: a delay is returned as a value and performed by the caller after the
// mutex is released. Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/core/types.hpp>

namespace crd
{

// Which read is asking.
enum class IoFaultOp : u8
{
    ReadWhole = 0U, // read_file_binary: the whole file (size 0: not known before the read)
    ReadRange = 1U, // read_file_range: `size` bytes at `offset`
    AsyncRead = 2U, // an AsyncFile completion job: `size` bytes at `offset`, completed through a status
};

// What the read must do.
enum class IoFaultAction : u8
{
    Proceed = 0U, // read normally
    Fail    = 1U, // the read fails before it delivers anything
    Short   = 2U, // the read delivers `value` bytes (fewer than asked) and then stops
    Delay   = 3U, // the read completes normally `value` nanoseconds late
    Cancel  = 4U, // the completion reports cancelled and delivers nothing (an AsyncRead only)
};

struct IoFault
{
    IoFaultAction action = IoFaultAction::Proceed;
    u64           value  = 0U; // Short: bytes delivered; Delay: nanoseconds; otherwise 0
};

using IoFaultHook = IoFault (*)(void* user, IoFaultOp op, const char* path, usize path_bytes, u64 offset,
                                u64 size) noexcept;

// False in a shipping build: the seam is compiled out.
[[nodiscard]] bool fault_seams_compiled() noexcept;

// Install `hook`. Returns false, changing nothing, when the seam is compiled out, `hook` is null or a hook is
// already installed.
[[nodiscard]] bool install_io_fault_hook(IoFaultHook hook, void* user) noexcept;

// Remove the hook. When it returns, no call of the old hook is in progress.
void uninstall_io_fault_hook() noexcept;

// What the read described by the arguments must do (Proceed when no hook is installed or the seam is compiled out).
[[nodiscard]] IoFault consult_io_fault(IoFaultOp op, const char* path, usize path_bytes, u64 offset, u64 size) noexcept;

} // namespace crd
