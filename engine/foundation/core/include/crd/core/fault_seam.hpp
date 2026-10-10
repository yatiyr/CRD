#pragma once

// crd-core -- the process-wide fault seams (DIAG.9b): one for file reads, one for GPU queue submissions.
//
// The file reads every engine module shares (crd-platform's read_file_binary, read_file_text, read_file_range and
// AsyncFile's completion jobs) consult the I/O seam before they touch the file. With no hook installed the consult is
// one atomic load and the read proceeds. A diagnostics host installs one hook (crd-perf's seeded FaultInjector
// provides FaultInjector::io_hook) to fail a read, cut it short, delay its completion or cancel it, so a test can drive
// the real recovery paths of load, cook, reload and teardown without a broken disk.
//
// The GPU providers' queue submissions (the Vulkan completion seam's vk_submit and DX12's dx12_submit) consult the
// GPU seam before any work of the submission is queued. Its hook (FaultInjector::gpu_hook) can answer LoseDevice:
// the provider then takes its own device-loss path as if the runtime had reported the loss, and labels the loss
// Simulated (Vulkan records VK_ERROR_DEVICE_LOST; DX12 calls ID3D12Device5::RemoveDevice, so every later call sees
// a removed device). Only a submission is consulted: a loss injected at a wait would let a Vulkan caller free memory
// that work already queued still uses, since Vulkan cannot stop queued work.
//
// Both seams are a build capability. A shipping build (CRD_SHIPPING) compiles them out: installing is refused and
// every consult answers Proceed, so no request, authored asset or host can inject a fault there.
//
// One hook per seam. A hook runs under its seam's mutex, so uninstalling returns only after every call of the old hook
// in progress has finished; the hook's user state may be destroyed after that. The I/O hook must not read a file
// through the seam, the GPU hook must not submit GPU work, and neither may block: a delay is returned as a value and
// performed by the caller after the mutex is released. Contract: docs/design/runtime-diagnostics.md (DIAG.9b);
// ADR-0133.

#include <crd/core/types.hpp>

namespace crd
{

// Which read is asking.
enum class IoFaultOp : u8
{
    ReadWhole = 0U, // read_file_binary or read_file_text: the whole file (size 0: not known before the read)
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

// Which GPU call is asking.
enum class GpuFaultOp : u8
{
    Submit = 0U, // a queue submission, before any of its work is queued
};

// What the GPU call must do.
enum class GpuFaultAction : u8
{
    Proceed    = 0U, // submit normally
    LoseDevice = 1U, // queue nothing; the provider takes its device-loss path and labels the loss Simulated
};

// `operation` is the provider's static label of the call (for example "compute submit").
using GpuFaultHook = GpuFaultAction (*)(void* user, GpuFaultOp op, const char* operation) noexcept;

// Install `hook`. Returns false, changing nothing, when the seam is compiled out, `hook` is null or a hook is
// already installed.
[[nodiscard]] bool install_gpu_fault_hook(GpuFaultHook hook, void* user) noexcept;

// Remove the hook. When it returns, no call of the old hook is in progress.
void uninstall_gpu_fault_hook() noexcept;

// What the GPU call must do (Proceed when no hook is installed or the seam is compiled out).
[[nodiscard]] GpuFaultAction consult_gpu_fault(GpuFaultOp op, const char* operation) noexcept;

} // namespace crd
