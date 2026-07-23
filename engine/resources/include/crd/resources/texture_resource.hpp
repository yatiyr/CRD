#pragma once

// texture_resource.hpp — RET-3 (ADR-0105): the CPU-side cooked texture, RE-HOMED from crd-renderer into crd-resources
// (the GPU-free home — ADR-0042's loader posture survives the retirement; its rhi upload half died with GEO-3 stage 4:
// `IRasterContext::create_texture_from_mips` is the upload path now). The loader's payload heap is INJECTABLE — pass a
// `StreamingCategoryAllocator` view and every loaded texture lives in the ADR-0085 resident store under the streaming
// budgets (the "first real streaming consumer" that ADR planned for); default = an owned thread-safe TLSF heap.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/loader.hpp>

namespace crd::resources
{

// On-disk byte values — never reorder (CRDR format spec, ADR-0042).
enum class TextureFormat : crd::u8
{
    RGBA8Unorm     = 0,
    BC7Unorm       = 1,
    BC7UnormSrgb   = 2,
    RGBA8UnormSrgb = 3, // GEO-3 (appended): sRGB-authored color — mips cooked in LINEAR space, sampled via sRGB view
};

// One mip level of a texture. Owns its pixel bytes.
// No default constructor: Array<MipLevel> cannot use resize() — use push_back.
struct MipLevel
{
    crd::u32                        width  = 0;
    crd::u32                        height = 0;
    crd::containers::Array<crd::u8> pixels;

    explicit MipLevel(crd::memory::IAllocator* a) : pixels(a) {}

    MipLevel(const MipLevel&)            = delete;
    MipLevel& operator=(const MipLevel&) = delete;
    MipLevel(MipLevel&&)                 = default;
    MipLevel& operator=(MipLevel&&)      = default;
};

// CPU-side cooked texture. mips[0] = full-resolution, mips[N-1] = 1×1. The GPU seam consumes the chain VERBATIM
// (`create_texture_from_mips` — the cook's linear-space-filtered levels are authoritative, never re-derived).
struct TextureResource
{
    TextureFormat                    format    = TextureFormat::RGBA8Unorm;
    crd::u32                         mip_count = 0;
    crd::containers::Array<MipLevel> mips;

    explicit TextureResource(crd::memory::IAllocator* a) : mips(a) {}

    TextureResource(const TextureResource&)            = delete;
    TextureResource& operator=(const TextureResource&) = delete;
    TextureResource(TextureResource&&)                 = default;
    TextureResource& operator=(TextureResource&&)      = default;
};

// ILoader for 'TXTR' → TextureResource. Payload heap injectable (see the header comment); the injected allocator
// must be safe under concurrent async loads (a StreamingCategoryAllocator view is — its resident path is serialized).
class TextureResourceLoader final : public ILoader
{
public:
    TextureResourceLoader() = default;
    explicit TextureResourceLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }

    [[nodiscard]] crd::u32 type_fourcc() const noexcept override;
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 2U; } // v2 = the crd-resources re-home
    [[nodiscard]] void*    load(const LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    // The default heap: concurrent async loads hit this single loader instance from worker fibers; the wrapper
    // serializes the TLSF heap while keeping per-type pool locality.
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

class ResourceManager;

// Register the TXTR loader. `payload_alloc` = nullptr → the loader's owned heap; a StreamingCategoryAllocator view →
// budgeted resident-store payloads (ADR-0085).
void register_texture_loader(ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::resources
