// openpbr_material.cpp — the 'PBRM' build + load paths. See openpbr_material.hpp for the artifact contract.

#include <crd/resources/openpbr_material.hpp>

#include <crd/containers/span.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::resources
{

crd::containers::Array<crd::u8> pbrm_build(const PbrmParams& params, const PbrmTextures& textures, const ResourceId& id,
                                           crd::memory::IAllocator* alloc)
{
    CrdrWriter writer(alloc, id, kFourCC_PBRM);
    writer.add_chunk(kFourCC_PbrmPrms,
                     crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&params), sizeof(params)));
    writer.add_chunk(kFourCC_PbrmTexs,
                     crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(&textures), sizeof(textures)));
    return writer.finish();
}

void* OpenPbrMaterialLoader::load(const LoadContext& ctx)
{
    // parse SCRATCH on the owned heap; only the RESIDENT payload charges m_payload (the streaming-category rule)
    CrdrFile file(&m_owned);
    if (crdr_read(ctx.bytes, file, &m_owned) != CrdrError::Ok) { return nullptr; }
    if (file.type_fourcc != kFourCC_PBRM) { return nullptr; }

    const CrdrChunk* prms = crdr_find_chunk(file, kFourCC_PbrmPrms);
    const CrdrChunk* texs = crdr_find_chunk(file, kFourCC_PbrmTexs);
    if (prms == nullptr || prms->payload.size() < sizeof(PbrmParams)) { return nullptr; }
    if (texs == nullptr || texs->payload.size() < sizeof(PbrmTextures)) { return nullptr; }

    PbrmParams params;
    std::memcpy(&params, prms->payload.data(), sizeof(params));
    if (params.version != kPbrmVersion) { return nullptr; } // unknown version — never a silently-misread material

    void* raw = m_payload->try_allocate(sizeof(OpenPbrMaterial), alignof(OpenPbrMaterial));
    if (raw == nullptr) { return nullptr; } // over-budget on a streaming heap — graceful, never fatal
    auto* mat   = new (raw) OpenPbrMaterial();
    mat->params = params;
    std::memcpy(&mat->textures, texs->payload.data(), sizeof(mat->textures));
    return mat;
}

void OpenPbrMaterialLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto* mat = static_cast<OpenPbrMaterial*>(payload);
    mat->~OpenPbrMaterial();
    m_payload->deallocate(mat);
}

void register_openpbr_material_loader(ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<OpenPbrMaterialLoader>(payload_alloc));
}

} // namespace crd::resources
