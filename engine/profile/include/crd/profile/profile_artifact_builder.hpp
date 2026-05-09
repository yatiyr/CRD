#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::profile
{
// ProfileArtifactBuilder — emits a FINF/FRLE/FBND CRDR blob for a single
// cooked profile artifact (a `.profile.toml` file may contain multiple
// `[profile.NAME]` rules; the cooker flattens them into one PROF artifact).
//
// Test-only public API in v1n5 (mirrors v1k SceneArtifactBuilder + v1m1
// ObekArtifactBuilder + v1n1 PresetArtifactBuilder); the cooker handler
// will wrap it once the TOML reader lands.
//
// Usage:
//   ProfileArtifactBuilder b{alloc, 1U /* schema_version */, id};
//   b.add_rule(100U /* priority */,
//              crd::containers::ConstSpan<PredicateRecord>{predicates.data(), predicates.size()},
//              crd::containers::ConstSpan<ResourceId>{bundle.data(), bundle.size()});
//   b.add_rule(50U, ...);
//   auto bytes = b.build();
class ProfileArtifactBuilder
{
public:
    ProfileArtifactBuilder(crd::memory::IAllocator*    alloc,
                           crd::u32                    schema_version,
                           crd::resources::ResourceId  id) noexcept;

    ProfileArtifactBuilder(const ProfileArtifactBuilder&)            = delete;
    ProfileArtifactBuilder& operator=(const ProfileArtifactBuilder&) = delete;

    // Append one rule + its preset bundle. Order is preserved as written
    // (becomes the canonical FRLE / FBND order).
    void add_rule(crd::u32                                                  priority,
                  crd::containers::ConstSpan<PredicateRecord>               predicates,
                  crd::containers::ConstSpan<crd::resources::ResourceId>    apply_bundle);

    [[nodiscard]] crd::containers::Array<crd::u8> build() const;

private:
    struct PendingRule
    {
        crd::u32                                          priority{};
        crd::containers::Array<PredicateRecord>           predicates;
        crd::containers::Array<crd::resources::ResourceId> apply_bundle;

        explicit PendingRule(crd::memory::IAllocator* a)
            : predicates(a), apply_bundle(a)
        {
        }
    };

    crd::memory::IAllocator*               m_alloc;
    crd::u32                               m_schema_version;
    crd::resources::ResourceId             m_id;
    crd::containers::Array<PendingRule>    m_rules;
};

} // namespace crd::profile
