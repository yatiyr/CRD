#include <crd/shader/vulkan_spirv_compiler.hpp>

#include <crd/gpu/program.hpp>              // crd::gpu::ShaderStage
#include <crd/gpu/vulkan_shader_compile.hpp> // crd::gpu::compile_glsl_to_spirv
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace crd::shader
{
namespace
{
[[nodiscard]] crd::gpu::ShaderStage to_gpu_stage(Stage stage) noexcept
{
    switch (stage)
    {
        case Stage::Vertex:
            return crd::gpu::ShaderStage::Vertex;
        case Stage::Fragment:
            return crd::gpu::ShaderStage::Fragment;
        case Stage::Compute:
        default:
            return crd::gpu::ShaderStage::Compute;
    }
}

// The Vulkan implementation of the injected compiler seam (D-008 C2-e). Wraps `crd::gpu::compile_glsl_to_spirv` with
// `optimize == false` so the SPIR-V retains the `OpName`s + dead bindings the Effect frontend's spirv-reflect needs.
class VulkanSpirvCompiler final : public ISpirvCompiler
{
public:
    [[nodiscard]] bool compile(Stage stage, crd::containers::StringView source, crd::containers::StringView name,
                               crd::containers::Array<crd::u32>& out_words, crd::containers::String& error) override
    {
        crd::memory::IAllocator* alloc = crd::memory::default_allocator();
        const crd::gpu::ShaderCompileResult result =
            crd::gpu::compile_glsl_to_spirv(to_gpu_stage(stage), source, name, alloc, /*optimize*/ false);
        if (!result.ok)
        {
            error = result.error_message;
            return false;
        }

        // SPIR-V is a stream of 32-bit words; the byte count is a multiple of 4 by construction.
        const crd::usize word_count = result.spirv.size() / sizeof(crd::u32);
        out_words.resize(word_count);
        if (word_count > 0) { std::memcpy(out_words.data(), result.spirv.data(), word_count * sizeof(crd::u32)); }
        return true;
    }
};
} // namespace

std::unique_ptr<ISpirvCompiler> create_vulkan_spirv_compiler()
{
    return std::make_unique<VulkanSpirvCompiler>();
}
} // namespace crd::shader
