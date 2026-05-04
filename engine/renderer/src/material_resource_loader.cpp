#include <crd/renderer/material_resource_loader.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>

namespace crd::renderer
{
namespace
{

constexpr crd::u32 kMaterialLoaderVersion = 1U;

// META chunk layout (32 bytes):
//   [0..7]   vertex_shader uuid_hi
//   [8..15]  vertex_shader uuid_lo
//   [16..23] fragment_shader uuid_hi
//   [24..31] fragment_shader uuid_lo

constexpr crd::usize kMetaChunkSize = 32U;

class MaterialResourceLoaderImpl final : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override
    {
        return crd::resources::kFourCC_MATR;
    }

    [[nodiscard]] crd::u32 loader_version() const noexcept override
    {
        return kMaterialLoaderVersion;
    }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override
    {
        crd::resources::CrdrFile file(&m_alloc);
        if (crd::resources::crdr_read(ctx.bytes, file, &m_alloc) != crd::resources::CrdrError::Ok)
        {
            return nullptr;
        }

        const crd::resources::CrdrChunk* meta =
            crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_META);
        if (meta == nullptr || meta->payload.size() < kMetaChunkSize)
        {
            return nullptr;
        }

        crd::resources::ResourceId vert_id;
        crd::resources::ResourceId frag_id;
        std::memcpy(&vert_id.hi, meta->payload.data() +  0, 8);
        std::memcpy(&vert_id.lo, meta->payload.data() +  8, 8);
        std::memcpy(&frag_id.hi, meta->payload.data() + 16, 8);
        std::memcpy(&frag_id.lo, meta->payload.data() + 24, 8);

        if (vert_id.is_null() || frag_id.is_null())
        {
            return nullptr;
        }

        // Transitive loads — mutex is NOT held during dispatch, so no deadlock.
        auto vert_handle = ctx.manager->load_sync<crd::shader::ShaderResource>(vert_id);
        auto frag_handle = ctx.manager->load_sync<crd::shader::ShaderResource>(frag_id);

        if (!vert_handle.is_ready() || !frag_handle.is_ready())
        {
            return nullptr;
        }

        void* raw = m_alloc.allocate(sizeof(MaterialResource), alignof(MaterialResource));
        auto* res = new (raw) MaterialResource{};
        res->vertex_shader   = std::move(vert_handle);
        res->fragment_shader = std::move(frag_handle);
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* res = static_cast<MaterialResource*>(payload);
        res->~MaterialResource();
        m_alloc.deallocate(res);
    }

private:
    crd::memory::MallocAllocator m_alloc;
};

} // anonymous namespace

void register_material_loader(crd::resources::ResourceManager* rm)
{
    rm->register_loader(std::make_unique<MaterialResourceLoaderImpl>());
}

} // namespace crd::renderer
