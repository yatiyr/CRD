#include <crd/shader/shader_resource_loader.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_manager.hpp>

#include <spirv_reflect.h>

#include <cstring>
#include <memory>

namespace crd::shader
{
namespace
{

constexpr crd::u32 kShaderLoaderVersion = 1U;

[[nodiscard]] crd::rhi::Format to_rhi_format_local(SpvReflectFormat format) noexcept
{
    switch (format)
    {
        case SPV_REFLECT_FORMAT_R32G32_SFLOAT:
            return crd::rhi::Format::R32G32Sfloat;
        case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:
            return crd::rhi::Format::R32G32B32Sfloat;
        case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT:
            return crd::rhi::Format::R8G8B8A8Unorm;
        default:
            return crd::rhi::Format::Undefined;
    }
}

void reflect_into(const crd::containers::ConstSpan<crd::u8>& spirv_bytes,
                  Stage stage, ShaderResource& out)
{
    if (spirv_bytes.empty())
    {
        return;
    }

    SpvReflectShaderModule mod{};
    const SpvReflectResult r =
        spvReflectCreateShaderModule(spirv_bytes.size(), spirv_bytes.data(), &mod);
    if (r != SPV_REFLECT_RESULT_SUCCESS)
    {
        return;
    }

    for (crd::u32 i = 0; i < mod.descriptor_binding_count; ++i)
    {
        const auto& b = mod.descriptor_bindings[i];
        out.descriptor_bindings.push_back({b.set, b.binding, b.count, stage_bit(stage)});
    }

    for (crd::u32 i = 0; i < mod.push_constant_block_count; ++i)
    {
        const auto& blk = mod.push_constant_blocks[i];
        out.push_constants.push_back({blk.offset, blk.size, stage_bit(stage)});
    }

    if (stage == Stage::Vertex)
    {
        for (crd::u32 i = 0; i < mod.input_variable_count; ++i)
        {
            const auto* var = mod.input_variables[i];
            if (var == nullptr || var->built_in >= 0)
            {
                continue;
            }
            out.vertex_attributes.push_back(
                {crd::containers::String(var->name != nullptr ? var->name : ""),
                 var->location, to_rhi_format_local(var->format), 0U});
        }
    }

    spvReflectDestroyShaderModule(&mod);
}

class ShaderResourceLoaderImpl final : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override
    {
        return crd::resources::kFourCC_SHDR;
    }

    [[nodiscard]] crd::u32 loader_version() const noexcept override
    {
        return kShaderLoaderVersion;
    }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override
    {
        crd::resources::CrdrFile file(&m_alloc);
        if (crd::resources::crdr_read(ctx.bytes, file, &m_alloc) != crd::resources::CrdrError::Ok)
        {
            return nullptr;
        }

        // Determine stage from which SPIRV chunk is present.
        const crd::resources::CrdrChunk* chunk = nullptr;
        Stage                            stage  = Stage::Vertex;

        chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_SPVV);
        if (chunk != nullptr)
        {
            stage = Stage::Vertex;
        }
        if (chunk == nullptr)
        {
            chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_SPVF);
            if (chunk != nullptr)
            {
                stage = Stage::Fragment;
            }
        }
        if (chunk == nullptr)
        {
            chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_SPVC);
            if (chunk != nullptr)
            {
                stage = Stage::Compute;
            }
        }

        if (chunk == nullptr)
        {
            return nullptr;
        }

        void* raw = m_alloc.allocate(sizeof(ShaderResource), alignof(ShaderResource));
        auto* res = new (raw) ShaderResource(&m_alloc);
        res->stage = stage;
        res->spirv.resize(chunk->payload.size());
        if (!chunk->payload.empty())
        {
            std::memcpy(res->spirv.data(), chunk->payload.data(), chunk->payload.size());
        }

        reflect_into(crd::containers::as_const_span(res->spirv), stage, *res);
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* res = static_cast<ShaderResource*>(payload);
        res->~ShaderResource();
        m_alloc.deallocate(res);
    }

private:
    // Concurrent async loads share this single loader instance; wrapper serializes the
    // single-threaded TLSF heap while keeping per-type pool locality (see texture loader).
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};
};

} // anonymous namespace

void register_shader_loader(crd::resources::ResourceManager* rm)
{
    rm->register_loader(std::make_unique<ShaderResourceLoaderImpl>());
}

} // namespace crd::shader
