#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::profile
{
// Profile — runtime representation of one rule + its preset bundle
// (ADR-0060 §1, §5). The cooker emits these from `[profile.NAME]` TOML
// blocks; the loader parses them back from the FRLE + FBND chunks.
//
// On disk the rule (priority + predicates) lives in FRLE and the bundle
// (apply_bundle) lives in FBND with a `rule_idx` link. In memory the loader
// joins them so consumers see a single Profile struct per cooked rule.
struct Profile
{
    crd::u32                                          priority = 0U;
    crd::containers::Array<PredicateRecord>           predicates;
    crd::containers::Array<crd::resources::ResourceId> apply_bundle;

    explicit Profile(crd::memory::IAllocator* a)
        : predicates(a), apply_bundle(a)
    {
    }

    Profile(const Profile&)            = delete;
    Profile& operator=(const Profile&) = delete;
    Profile(Profile&&) noexcept        = default;
    Profile& operator=(Profile&&) noexcept = default;
    ~Profile()                          = default;
};

} // namespace crd::profile
