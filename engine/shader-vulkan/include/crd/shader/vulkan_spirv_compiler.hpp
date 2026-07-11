#pragma once

// crd-shader-vulkan — the ONE bridge between the portable Effect runtime (`crd-shader`) and the Vulkan shading-language
// compiler (`crd-gpu-context-vulkan`). ADR-0103 / D-008 C2-e: `crd-shader` names no shading language and links no vendor
// compiler; it takes an injected `crd::shader::ISpirvCompiler`. This module provides the Vulkan implementation, wrapping
// `crd::gpu::compile_glsl_to_spirv`. It links BOTH crd-shader (the interface) and crd-gpu-context-vulkan (the compiler),
// so neither of those has to depend on the other — keeping the compute/rendering decoupling (gpu-context stays rhi-free).

#include <crd/shader/runtime.hpp> // crd::shader::ISpirvCompiler

#include <memory>

namespace crd::shader
{
// Create a GLSL→SPIR-V compiler backed by `crd::gpu::compile_glsl_to_spirv` (unoptimized — the Effect frontend reflects
// the output, so `OpName`s + dead bindings must survive). Feed it to `crd::shader::create_runtime`; the compiler must
// OUTLIVE the runtime (the runtime borrows it). Returns a valid object even if shaderc is missing at runtime — in that
// case `compile()` fails per-call with an error, matching the old inline behavior.
[[nodiscard]] std::unique_ptr<ISpirvCompiler> create_vulkan_spirv_compiler();
} // namespace crd::shader
