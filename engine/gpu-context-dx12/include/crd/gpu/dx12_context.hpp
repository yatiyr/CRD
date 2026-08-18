#pragma once

// crd-gpu-context-dx12 — the DIRECT3D 12 IGpuContext + the DX12 program-authoring seam (ADR-0099 / ADR-0103 / D-008 C4).
// The DX12 mirror of VulkanGpuContext: it mints Dx12GpuPrograms — opaque IGpuProgram handles carrying cooked DXIL — from
// EITHER already-cooked bytecode (the ship path / escape hatch, I2) OR a CKIR graph (the IR on-ramp: crd-kir emits HLSL,
// dxc lowers it to DXIL). No portable module ever holds the HLSL text or the DXIL bytes — a backend owns its language +
// vendor compiler PRIVATELY (I1/I2). WINDOWS-ONLY; the factory returns nullptr elsewhere / when D3D12 or dxc is absent.

#include <crd/gpu/context.hpp> // IGpuContext, IGpuProgram, ShaderStage, GpuBackend

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>

#include <memory>

namespace crd::gpu
{

// Concrete-DX12 view of an IGpuProgram. It retains its cooked DXIL so the DX12 raster context can build a graphics PSO
// (VS+FS) from it — the analog of VulkanGpuProgram::vk_spirv(). Portable code stays opaque to it; the DX12 raster context
// downcasts to reach dxil().
class Dx12GpuProgram : public IGpuProgram
{
public:
    [[nodiscard]] virtual crd::containers::ConstSpan<crd::u8> dxil() const noexcept = 0;
    // B1-f: true iff this (fragment) program reads SV_InnerCoverage — its graphics PSO MUST enable conservative raster (the
    // D3D12 rasterizer rejects SV_InnerCoverage with conservative OFF), so the raster context prebuilds it conservative.
    [[nodiscard]] virtual bool wants_conservative_raster() const noexcept = 0;
};

// Create a headless D3D12 gpu context (ADR-0099/0103): the DX12 program-authoring seam. Owns a D3D12 device (for the
// adapter identity + the "live device foundation" contract) and dxc. Returns nullptr if D3D12 or dxc is unavailable
// (non-Windows, no adapter, dxcompiler.dll missing). The returned object is a Dx12GpuContext behind the IGpuContext handle.
[[nodiscard]] std::unique_ptr<IGpuContext>
create_dx12_gpu_context(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// True iff the D3D12 DEFAULT adapter (the one `D3D12CreateDevice(nullptr, ...)` selects — what both the compute and
// raster/RT contexts run on) is a SOFTWARE adapter (WARP), reported via `DXGI_ADAPTER_FLAG_SOFTWARE`. GPU numerical
// tests use this to relax a hardware-tuned tolerance on WARP ONLY (its fp32 transcendentals / rasterization / RT
// diverge from real silicon by ~orders of magnitude more than hardware) while keeping the tight bar on real GPUs —
// e.g. GitHub-hosted runners have no GPU, so DX12 falls back to WARP. False if no device/adapter is available.
[[nodiscard]] bool dx12_default_adapter_is_software() noexcept;

// Mint a Dx12GpuProgram from cooked DXIL — the ONE program constructor (mirror of make_vulkan_program). DXIL is device-
// independent bytecode, so no device is needed: the program just copies + owns the bytes. Returns nullptr on empty input.
[[nodiscard]] std::unique_ptr<IGpuProgram>
make_dx12_program(ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked_dxil, crd::memory::IAllocator* alloc);

// DXIL compile output (mirror of ShaderCompileResult). The DXIL bytes are the signed D3D12-consumable container.
struct DxilCompileResult
{
    bool                            ok = false;
    crd::containers::Array<crd::u8> dxil;
    crd::containers::String         error_message;

    explicit DxilCompileResult(crd::memory::IAllocator* a = crd::memory::default_allocator()) : dxil(a), error_message(a)
    {
    }
};

// HLSL → DXIL via dxc (vs_6_0/ps_6_0/cs_6_0; entry `main`, or `cs_main` for Compute — the geometry-kernel convention).
// The DX12 analog of compile_hlsl_to_spirv, exposed publicly so backend conformance tests can cook trivial shaders. Returns
// `ok == false` + an error_message when dxc is unavailable (dxcompiler.dll missing) — never a link error, so tests soft-skip.
[[nodiscard]] DxilCompileResult compile_hlsl_to_dxil(
    ShaderStage stage, crd::containers::StringView source, crd::containers::StringView name,
    crd::memory::IAllocator* a = crd::memory::default_allocator());

// CEIR-20c-1 (D3D12 Work Graphs): compile a NODE LIBRARY (one or more [Shader("node")] functions — the
// emit_work_graph_node_hlsl output) to signed DXIL. Target `lib_6_8`, no entry point (the state object names the entry).
// Requires a Work-Graphs-capable dxcompiler.dll on the load path. `ok == false` + error_message if dxc is unavailable.
[[nodiscard]] DxilCompileResult compile_work_graph_library_to_dxil(
    crd::containers::StringView source, crd::containers::StringView name,
    crd::memory::IAllocator* a = crd::memory::default_allocator());

} // namespace crd::gpu
