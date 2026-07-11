#pragma once

// crd-gpu-context-vulkan — GLSL/HLSL → SPIR-V, the VULKAN backend's PRIVATE shader compilers (ADR-0103). These moved
// out of `crd-shader` (which must not know any shading language) into the Vulkan backend that owns them. The GLSL/HLSL
// TEXT and the SPIR-V BYTES live only here, between our emitter and the vendor compiler (shaderc / dxc). No portable
// module calls these; only Vulkan-target consumers (kir-vulkan, the asset cooker's Vulkan cook path, backend
// conformance tests) do. Both loaders are dynamic — a missing shaderc/dxc yields `ok == false`, never a link error.

#include <crd/gpu/program.hpp> // ShaderStage

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::gpu
{

// SPIR-V compile output. Same shape as the old `crd::shader::CompileResult`, now Vulkan-backend-local.
struct ShaderCompileResult
{
    bool                            ok = false;
    crd::containers::Array<crd::u8> spirv;
    crd::containers::String         error_message;

    explicit ShaderCompileResult(crd::memory::IAllocator* a = crd::memory::default_allocator())
        : spirv(a), error_message(a)
    {
    }
};

// GLSL → SPIR-V via shaderc (Vulkan 1.3 / SPIR-V 1.6 target env). `name` is used only in diagnostics. Returns
// `ok == false` + an `error_message` on failure (including "shaderc not loaded"). Only Compute/Vertex/Fragment are
// implemented; any other `ShaderStage` is refused with an error (never miscompiled).
//
// `optimize` (default true) runs shaderc's spirv-opt PERFORMANCE passes — right for a runnable kernel/pipeline. Pass
// `false` for the REFLECTION path (D-008 C2-e crd-shader Effect frontend): performance passes strip `OpName`
// decorations + dead bindings, which spirv-reflect needs; `level_zero` preserves them.
[[nodiscard]] ShaderCompileResult compile_glsl_to_spirv(
    ShaderStage stage, crd::containers::StringView source, crd::containers::StringView name,
    crd::memory::IAllocator* a = crd::memory::default_allocator(), bool optimize = true);

// HLSL → SPIR-V via dxc (`-spirv`, Vulkan 1.3 target env). Same contract. When dxc headers were absent at build time
// (`CRD_HAS_DXC=0`, e.g. Linux without dxc-dev) this returns `ok == false` so conformance tests soft-skip.
[[nodiscard]] ShaderCompileResult compile_hlsl_to_spirv(
    ShaderStage stage, crd::containers::StringView source, crd::containers::StringView name,
    crd::memory::IAllocator* a = crd::memory::default_allocator());

} // namespace crd::gpu
