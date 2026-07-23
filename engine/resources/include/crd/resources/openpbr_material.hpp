#pragma once

// openpbr_material.hpp — GEO-3 stage 4 (D-007 row 68): the NEW-WORLD material resource. An imported material AUTHORS
// into this OpenPBR parameter surface + texture ResourceId slots; the B8/gpu-context renderer consumes it (the CKIR
// material profile's OpenPBR slab, ADR-0101/0102) — foreign material representations never travel past the import
// seam, and NOTHING here touches the retiring crd-renderer 'MATR' path (ADR-0105: this type is GPU-free and lives in
// crd-resources, the RET-3 home, from day one).
//
// CRDR artifact, type_fourcc 'PBRM':
//   'PBRP' — the parameter block (PbrmParams, version-pinned, APPEND-ONLY evolution)
//   'PBRT' — the texture slots (PbrmTextures: 5 × ResourceId; null = unbound). Slot COLOR SPACE is decided at
//            texture cook (baseColor/emissive → sRGB, MR/occlusion/normal → linear) — the material only references.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

// NOLINTBEGIN(readability-identifier-naming) — the kFourCC_* names mirror on-disk chunk mnemonics (the repo-wide
// FourCC convention, crdr.hpp).
inline constexpr crd::u32 kFourCC_PBRM     = make_fourcc('P', 'B', 'R', 'M');
inline constexpr crd::u32 kFourCC_PbrmPrms = make_fourcc('P', 'B', 'R', 'P');
inline constexpr crd::u32 kFourCC_PbrmTexs = make_fourcc('P', 'B', 'R', 'T');
// NOLINTEND(readability-identifier-naming)

inline constexpr crd::u32 kPbrmVersion = 1U;

// The on-disk parameter block ('PBRP', 60 bytes, little-endian). Angles/lengths dimensionless; colors LINEAR RGB
// (the glTF/OpenPBR source conventions). Fields APPEND at the end only — the version gates readers.
struct PbrmParams
{
    crd::u32 version       = kPbrmVersion;
    crd::f32 base_color[3] = {1.0F, 1.0F, 1.0F};
    crd::f32 base_alpha    = 1.0F;
    crd::f32 metallic      = 0.0F;
    crd::f32 roughness     = 1.0F;
    crd::f32 emissive[3]   = {0.0F, 0.0F, 0.0F};
    crd::f32 emissive_strength  = 1.0F;
    crd::f32 ior                = 1.5F;
    crd::f32 transmission       = 0.0F;
    crd::f32 normal_scale       = 1.0F;
    crd::f32 occlusion_strength = 1.0F;
};
static_assert(sizeof(PbrmParams) == 60, "PbrmParams is the on-disk 'PBRP' layout — append-only, size gates version 1");

// The on-disk texture-slot block ('PBRT', 80 bytes): 5 slots, glTF/OpenPBR core set. Null id = unbound.
struct PbrmTextures
{
    ResourceId base_color{};
    ResourceId metallic_roughness{}; // glTF packing: G = roughness, B = metallic
    ResourceId normal{};
    ResourceId occlusion{};
    ResourceId emissive{};
};
static_assert(sizeof(PbrmTextures) == 80, "PbrmTextures is the on-disk 'PBRT' layout");

// The loaded payload: the two pinned blocks, plus room for future non-POD state (allocator kept for that evolution).
struct OpenPbrMaterial
{
    PbrmParams   params;
    PbrmTextures textures;
};

// Build the cooked 'PBRM' CRDR bytes (the cook side — pure data, no GPU, no filesystem).
[[nodiscard]] crd::containers::Array<crd::u8> pbrm_build(const PbrmParams& params, const PbrmTextures& textures,
                                                         const ResourceId& id, crd::memory::IAllocator* alloc);

// ILoader for 'PBRM' → OpenPbrMaterial. Rejects unknown versions and short chunks (never a partial material).
// Payload heap injectable (RET-3, the texture/mesh-loader pattern): a StreamingCategoryAllocator view puts material
// payloads under the ADR-0085 streaming budgets.
class OpenPbrMaterialLoader : public ILoader
{
public:
    OpenPbrMaterialLoader() = default;
    explicit OpenPbrMaterialLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }

    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_PBRM; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    // Concurrent async loads hit this single loader instance from worker fibers; the thread-safe wrapper
    // serializes the TLSF heap (the texture-loader pattern). Scratch parses on m_owned; payloads go to m_payload.
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

// Register the loader on a ResourceManager. `payload_alloc` = nullptr → the loader's owned heap; a
// StreamingCategoryAllocator view → budgeted resident-store payloads (ADR-0085).
class ResourceManager;
void register_openpbr_material_loader(ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::resources
