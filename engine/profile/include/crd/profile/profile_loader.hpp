#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/loader.hpp>

namespace crd::profile
{
// ProfileLoader — ILoader for the 'PROF' container. Validates and parses
// FINF / FRLE / FBND chunks into a ProfileResource carrying an
// Array<Profile> in canonical order.
//
// Validation at load time:
//   - CRDR type_fourcc must be 'PROF'.
//   - FINF chunk must be present and exactly 16 bytes; schema_version must
//     equal the loader's expected version (1 in v1n5).
//   - FRLE chunk must be present; total bytes must equal the cumulative
//     sum of (header + predicate_count * sizeof(PredicateRecord)) across
//     `rule_count` rules.
//   - FBND chunk must be present; total bytes must equal the cumulative
//     sum of (header + preset_id_count * sizeof(ResourceId)) across
//     `bundle_count` bundles.
//   - rule_count must equal bundle_count (v1n5 enforces 1 bundle per rule).
//
// On any failure the loader returns nullptr → LoadState::Failed.
class ProfileLoader : public crd::resources::ILoader
{
public:
    explicit ProfileLoader(crd::memory::IAllocator* alloc) noexcept;

    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override;
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return m_schema_version; }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override;
    void                unload(void* payload) noexcept override;

private:
    crd::memory::IAllocator* m_alloc{};
    crd::u32                 m_schema_version = 1U;
};

} // namespace crd::profile
