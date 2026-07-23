// texture_resource_loader.cpp — RET-3: the TXTR loader, re-homed from crd-renderer (ADR-0105). See texture_resource.hpp.

#include <crd/resources/texture_resource.hpp>

#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::resources
{

namespace
{
// HEAD chunk layout (16 bytes):
//   +0  u32 width · +4 u32 height · +8 u32 mip_count · +12 u8 format (TextureFormat byte) · +13 u8[3] zero
constexpr crd::usize kHeadChunkSize = 16U;
constexpr crd::u32   kMaxMipLevels  = 16U;
} // namespace

crd::u32 TextureResourceLoader::type_fourcc() const noexcept { return kFourCC_TXTR; }

void* TextureResourceLoader::load(const LoadContext& ctx)
{
    // parse SCRATCH on the owned heap (transient — dies with this call); only the RESIDENT payload goes to
    // m_payload, so a streaming category is charged for exactly what stays resident
    CrdrFile file(&m_owned);
    if (crdr_read(ctx.bytes, file, &m_owned) != CrdrError::Ok) { return nullptr; }

    const CrdrChunk* head = crdr_find_chunk(file, kFourCC_HEAD);
    if (head == nullptr || head->payload.size() < kHeadChunkSize) { return nullptr; }

    crd::u32 width     = 0;
    crd::u32 height    = 0;
    crd::u32 mip_count = 0;
    crd::u8  fmt_byte  = 0;
    std::memcpy(&width, head->payload.data() + 0, sizeof(crd::u32));
    std::memcpy(&height, head->payload.data() + 4, sizeof(crd::u32));
    std::memcpy(&mip_count, head->payload.data() + 8, sizeof(crd::u32));
    std::memcpy(&fmt_byte, head->payload.data() + 12, sizeof(crd::u8));

    if (width == 0 || height == 0 || mip_count == 0 || mip_count > kMaxMipLevels) { return nullptr; }
    if (fmt_byte > static_cast<crd::u8>(TextureFormat::RGBA8UnormSrgb)) { return nullptr; }
    const TextureFormat fmt = static_cast<TextureFormat>(fmt_byte);

    void* raw = m_payload->try_allocate(sizeof(TextureResource), alignof(TextureResource));
    if (raw == nullptr) { return nullptr; } // over-budget on a streaming heap — graceful, never fatal
    auto* res      = new (raw) TextureResource(m_payload);
    res->format    = fmt;
    res->mip_count = mip_count;

    crd::u32 mip_w = width;
    crd::u32 mip_h = height;
    for (crd::u32 lvl = 0U; lvl < mip_count; ++lvl)
    {
        const crd::u32   mip_cc    = make_mip_fourcc(static_cast<crd::u8>(lvl));
        const CrdrChunk* mip_chunk = crdr_find_chunk(file, mip_cc);
        if (mip_chunk == nullptr)
        {
            res->~TextureResource();
            m_payload->deallocate(res);
            return nullptr;
        }
        if (fmt == TextureFormat::RGBA8Unorm || fmt == TextureFormat::RGBA8UnormSrgb)
        {
            const crd::usize expected = static_cast<crd::usize>(mip_w) * static_cast<crd::usize>(mip_h) * 4U;
            if (mip_chunk->payload.size() != expected)
            {
                res->~TextureResource();
                m_payload->deallocate(res);
                return nullptr;
            }
        }
        MipLevel level(m_payload);
        level.width  = mip_w;
        level.height = mip_h;
        level.pixels.resize(mip_chunk->payload.size());
        std::memcpy(level.pixels.data(), mip_chunk->payload.data(), mip_chunk->payload.size());
        res->mips.push_back(std::move(level));

        mip_w = (mip_w > 1U) ? mip_w / 2U : 1U;
        mip_h = (mip_h > 1U) ? mip_h / 2U : 1U;
    }
    return res;
}

void TextureResourceLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto* res = static_cast<TextureResource*>(payload);
    res->~TextureResource();
    m_payload->deallocate(res);
}

void register_texture_loader(ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<TextureResourceLoader>(payload_alloc));
}

} // namespace crd::resources
