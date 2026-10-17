// Pack checks shared by the SCEN and OBEK loaders and instantiation; see pack_validation.hpp.

#include "pack_validation.hpp"

namespace crd::scene
{
const char* pack_refusal_name(PackRefusal refusal) noexcept
{
    switch (refusal)
    {
        case PackRefusal::None:
            return "None";
        case PackRefusal::MalformedPack:
            return "MalformedPack";
        case PackRefusal::ComponentSizeMismatch:
            return "ComponentSizeMismatch";
        case PackRefusal::ComponentAlignmentMismatch:
            return "ComponentAlignmentMismatch";
        case PackRefusal::ComponentVersionMismatch:
            return "ComponentVersionMismatch";
        case PackRefusal::RelationCycle:
            return "RelationCycle";
    }
    return "Unknown";
}

namespace pack_detail
{
ComponentId find_by_fourcc(const ComponentRegistry& registry, crd::u32 fourcc) noexcept
{
    if (fourcc == 0U)
    {
        return ComponentId{};
    }
    for (crd::u32 i = 0U; i < registry.size(); ++i)
    {
        const ComponentId cid{static_cast<crd::u16>(i)};
        const ComponentInfo* info = registry.info(cid);
        if (info != nullptr && info->serialize.fourcc == fourcc)
        {
            return cid;
        }
    }
    return ComponentId{};
}

ComponentId restored_component(const ComponentRegistry& registry, crd::u32 fourcc, bool skip_dont_inherit) noexcept
{
    const ComponentId id = find_by_fourcc(registry, fourcc);
    if (id.is_null())
    {
        return id;
    }
    const ComponentInfo& info = registry.require_info(id);
    if (info.is_relation || (skip_dont_inherit && info.inherit_policy == InheritPolicy::DontInherit))
    {
        return ComponentId{};
    }
    return id;
}
} // namespace pack_detail
} // namespace crd::scene
