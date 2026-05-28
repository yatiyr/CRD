#include <crd/renderer/texture_resource_loader.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/renderer/texture_resource.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>

namespace crd::renderer
{
namespace
{

constexpr crd::u32   kTextureLoaderVersion = 1U;

// HEAD chunk layout (16 bytes):
//   +0  u32 width
//   +4  u32 height
//   +8  u32 mip_count
//   +12 u8  format  (TextureFormat byte value)
//   +13 u8[3] padding (zero)
constexpr crd::usize kHeadChunkSize = 16U;
constexpr crd::u32   kMaxMipLevels  = 16U;

class TextureResourceLoaderImpl final : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override
    {
        return crd::resources::kFourCC_TXTR;
    }

    [[nodiscard]] crd::u32 loader_version() const noexcept override
    {
        return kTextureLoaderVersion;
    }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override
    {
        crd::resources::CrdrFile file(&m_alloc);
        if (crd::resources::crdr_read(ctx.bytes, file, &m_alloc) != crd::resources::CrdrError::Ok)
        {
            return nullptr;
        }

        const crd::resources::CrdrChunk* head =
            crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_HEAD);
        if (head == nullptr || head->payload.size() < kHeadChunkSize)
        {
            return nullptr;
        }

        crd::u32 width     = 0;
        crd::u32 height    = 0;
        crd::u32 mip_count = 0;
        crd::u8  fmt_byte  = 0;
        std::memcpy(&width,     head->payload.data() +  0, sizeof(crd::u32));
        std::memcpy(&height,    head->payload.data() +  4, sizeof(crd::u32));
        std::memcpy(&mip_count, head->payload.data() +  8, sizeof(crd::u32));
        std::memcpy(&fmt_byte,  head->payload.data() + 12, sizeof(crd::u8));

        if (width == 0 || height == 0 || mip_count == 0 || mip_count > kMaxMipLevels)
        {
            return nullptr;
        }
        if (fmt_byte > static_cast<crd::u8>(TextureFormat::BC7UnormSrgb))
        {
            return nullptr;
        }
        const TextureFormat fmt = static_cast<TextureFormat>(fmt_byte);

        void* raw = m_alloc.allocate(sizeof(TextureResource), alignof(TextureResource));
        auto* res = new (raw) TextureResource(&m_alloc);
        res->format    = fmt;
        res->mip_count = mip_count;

        crd::u32 mip_w = width;
        crd::u32 mip_h = height;

        for (crd::u32 lvl = 0U; lvl < mip_count; ++lvl)
        {
            const crd::u32 mip_cc =
                crd::resources::make_mip_fourcc(static_cast<crd::u8>(lvl));
            const crd::resources::CrdrChunk* mip_chunk =
                crd::resources::crdr_find_chunk(file, mip_cc);

            if (mip_chunk == nullptr)
            {
                res->~TextureResource();
                m_alloc.deallocate(res);
                return nullptr;
            }

            if (fmt == TextureFormat::RGBA8Unorm)
            {
                const crd::usize expected =
                    static_cast<crd::usize>(mip_w) *
                    static_cast<crd::usize>(mip_h) * 4U;
                if (mip_chunk->payload.size() != expected)
                {
                    res->~TextureResource();
                    m_alloc.deallocate(res);
                    return nullptr;
                }
            }

            MipLevel level(&m_alloc);
            level.width  = mip_w;
            level.height = mip_h;
            level.pixels.resize(mip_chunk->payload.size());
            std::memcpy(level.pixels.data(),
                        mip_chunk->payload.data(),
                        mip_chunk->payload.size());
            res->mips.push_back(std::move(level));

            mip_w = (mip_w > 1U) ? mip_w / 2U : 1U;
            mip_h = (mip_h > 1U) ? mip_h / 2U : 1U;
        }

        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* res = static_cast<TextureResource*>(payload);
        res->~TextureResource();
        m_alloc.deallocate(res);
    }

private:
    // Concurrent async loads call load()/unload() on this single loader instance from
    // different worker fibers; the wrapper serializes the otherwise single-threaded
    // TLSF heap. Per-type pool locality is kept; lock is held only per alloc/free call.
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};
};

} // anonymous namespace

void register_texture_loader(crd::resources::ResourceManager* rm)
{
    rm->register_loader(std::make_unique<TextureResourceLoaderImpl>());
}

} // namespace crd::renderer
