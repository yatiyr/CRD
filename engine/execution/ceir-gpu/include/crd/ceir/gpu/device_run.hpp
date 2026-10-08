#pragma once

// crd-ceir-gpu -- run a device program's block on a GPU from host data (DIAG.9a): the GPU half of a device executor
// for crd-ceir-cook's device records (crd/ceir/cook/device_replay.hpp).
//
// run_device_block lowers the block (lower_region), reads each dispatched kernel's CKIR text (ckir_read), compiles it
// with the host's hook (one pipeline per dispatch, with that dispatch's binding count), runs the list on the host
// buffers with execute_lowered_host and reports which dispatch an error is blamed on. A kernel whose text is missing,
// does not read or does not compile fails the run (false, nothing ran); an execution error is the run's result.
//
// crd-ceir-gpu names no backend and no record: the compile hook is the host's (GLSL to SPIR-V on Vulkan, HLSL on DX12),
// and the host wraps this call as a cook::DeviceExecutor with the compute context's adapter (crd-ceir-gpu does not link
// the cook bridge, which the renderer would then pull in). The rig must outlive every run; a run uses the compute
// context on the calling thread, so a host calls it only where that thread may submit to the context's queue.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/compute.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocator.hpp>

#include <memory>

namespace crd::ceir::gpu
{
// Compile one CKIR compute kernel with `bindings` storage-buffer bindings for the backend (nullptr: it does not build).
using KernelCompileFn = std::unique_ptr<crd::gpu::ComputePipeline> (*)(const crd::kir::KGraph& graph,
                                                                       const crd::kir::KEntry& entry, int bindings,
                                                                       void* user, memory::IAllocator* alloc);

// One dispatched kernel: its symbol (without '@') and its CKIR text (crd-kir's `ckir_write` form).
struct DeviceKernelText
{
    containers::StringView symbol;
    containers::StringView ckir;
};

struct DeviceRunRig
{
    crd::gpu::IComputeContext* device       = nullptr;
    KernelCompileFn            compile      = nullptr;
    void*                      compile_user = nullptr;
    memory::IAllocator*        alloc        = nullptr; // each run's lowering, kernel graphs and dispatch sites
    crd::u32                   runs         = 0U;      // runs started (evidence)
};

struct DeviceRunResult
{
    ExecuteError error    = ExecuteError::None;
    crd::u64     fault_op = 0U; // the stable id of the dispatch `error` is blamed on (0: none)
};

// Run `block` (a device program's top-level block in `ctx`) with `kernels` on `buffers` (one binding per declared
// buffer: each written buffer is read back into its data). False, with `reason` saying why, when the run could not
// start; true otherwise, whatever `out` says.
[[nodiscard]] bool run_device_block(Context& ctx, const Block& block, containers::ConstSpan<DeviceKernelText> kernels,
                                    containers::ConstSpan<HostBufferBinding> buffers, DeviceRunRig& rig,
                                    DeviceRunResult& out, containers::String& reason);
} // namespace crd::ceir::gpu
