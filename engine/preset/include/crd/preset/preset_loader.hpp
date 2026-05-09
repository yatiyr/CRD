#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/loader.hpp>

namespace crd::preset
{
// PresetLoader — runtime-configured ILoader. One instance per registered
// preset type; the configuration (FourCC, schema_version, payload_size) is
// captured at construction so PresetRegistry can mint a loader per
// register_type<T>() call without templating the loader itself.
//
// Validation at load time:
//   - CRDR type_fourcc must match the loader's configured FourCC.
//   - PINF chunk must be present, 16 bytes, and report a payload_size that
//     matches the loader's configured size_bytes (sanity check vs schema
//     drift).
//   - PDAT chunk must be present and exactly payload_size bytes.
//   - PCHN chunk is optional; if present it must be a u32 count + u32
//     reserved + PresetChainEntry[count].
//
// On any failure the loader returns nullptr → ResourceManager flips the
// handle to LoadState::Failed (no soft fallback in v1n1; that's the
// generic ILoader contract).
class PresetLoader : public crd::resources::ILoader
{
public:
    PresetLoader(crd::u32 fourcc,
                 crd::u32 schema_version,
                 crd::u32 payload_size,
                 crd::memory::IAllocator* alloc) noexcept;

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override { return m_fourcc; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return m_schema_version; }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override;
    void                unload(void* payload) noexcept override;

private:
    crd::u32                  m_fourcc{};
    crd::u32                  m_schema_version{};
    crd::u32                  m_payload_size{};
    crd::memory::IAllocator*  m_alloc{};
};

} // namespace crd::preset
