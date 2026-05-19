#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/shader/types.hpp>

namespace crd::shader
{

struct CompileResult
{
    bool                            ok = false;
    crd::containers::Array<crd::u8> spirv;
    crd::containers::String         error_message;

    explicit CompileResult(crd::memory::IAllocator* a = crd::memory::default_allocator())
        : spirv(a), error_message(a)
    {
    }
};

// Compile GLSL source text to SPIRV bytes.
// Loads shaderc_shared dynamically (VULKAN_SDK/Bin on Windows, LD path on Linux).
// Returns CompileResult::ok == false and an error_message on failure.
[[nodiscard]] CompileResult compile_glsl(
    Stage                            stage,
    crd::containers::StringView      source,
    crd::containers::StringView      name,
    crd::memory::IAllocator*         a = crd::memory::default_allocator());

// Compile HLSL source text to SPIR-V bytes via dxc with `-spirv` target.
// Loads dxcompiler dynamically (VULKAN_SDK/Bin/dxcompiler.dll on Windows,
// libdxcompiler.so on Linux). Defaults: shader model 6.0, entry name `main`
// (Vertex/Fragment) or `cs_main` (Compute), Vulkan 1.3 target environment.
// Returns CompileResult::ok == false and an error_message on failure
// (including library-not-found).
[[nodiscard]] CompileResult compile_hlsl(
    Stage                            stage,
    crd::containers::StringView      source,
    crd::containers::StringView      name,
    crd::memory::IAllocator*         a = crd::memory::default_allocator());

} // namespace crd::shader
