#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::preset
{
// PresetArtifactBuilder — emits a PINF/PDAT/PCHN CRDR blob for a single
// cooked preset.
//
// Test-only public API in v1n1 (matches v1k's SceneArtifactBuilder pattern);
// v1n's preset cooker handler will wrap this when concrete types ship in
// v1n2/v1n3 and the TOML reader is added.
//
// Usage:
//   PresetArtifactBuilder b{alloc, MyPreset::fourcc, MyPreset::version, id};
//   b.set_payload(crd::containers::ConstSpan<u8>{reinterpret_cast<const u8*>(&value),
//                                                 sizeof(value)});
//   b.add_chain_dependency(fnv1a_64("preset/base.preset.toml"),
//                          fnv1a_64(base_source_bytes));
//   auto bytes = b.build();
class PresetArtifactBuilder
{
public:
    PresetArtifactBuilder(crd::memory::IAllocator*    alloc,
                          crd::u32                    type_fourcc,
                          crd::u32                    schema_version,
                          crd::resources::ResourceId  id) noexcept;

    PresetArtifactBuilder(const PresetArtifactBuilder&)            = delete;
    PresetArtifactBuilder& operator=(const PresetArtifactBuilder&) = delete;

    // Set the PDAT payload (raw schema bytes). Replaces any previous payload.
    void set_payload(crd::containers::ConstSpan<crd::u8> bytes);

    // Append one PCHN entry. Order is preserved as written; cooker writes
    // these deepest-first to match the extends-resolution walk.
    void add_chain_dependency(crd::u64 path_hash, crd::u64 content_hash);

    [[nodiscard]] crd::containers::Array<crd::u8> build() const;

private:
    crd::memory::IAllocator*                  m_alloc;
    crd::u32                                  m_fourcc;
    crd::u32                                  m_schema_version;
    crd::resources::ResourceId                m_id;
    crd::containers::Array<crd::u8>           m_payload;
    crd::containers::Array<PresetChainEntry>  m_chain;
};

} // namespace crd::preset
