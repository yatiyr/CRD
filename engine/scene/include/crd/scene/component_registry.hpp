#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/relation.hpp>

#include <type_traits>
#include <typeinfo>
#include <utility>

namespace crd::scene
{
// Per-T type-identity tag.
//
// Each instantiation `ComponentTypeTag<T>` has a distinct `value` whose address
// is unique across the program. Using `&ComponentTypeTag<T>::value` as the
// key avoids RTTI on the registration hot path and works across translation
// units. typeid().name() is still used for debug-only ComponentInfo::name.
template <typename T> struct ComponentTypeTag
{
    static constexpr char value = 0;
};

template <typename T> [[nodiscard]] constexpr const void* component_type_key() noexcept
{
    return &ComponentTypeTag<T>::value;
}

// ---- Trait dispatch (overload set; unknown trait → compile error) -------

namespace detail
{
inline void apply_trait(ComponentInfo& info, StorageHint h) noexcept
{
    info.storage_hint = h;
}
inline void apply_trait(ComponentInfo& info, Replication r) noexcept
{
    info.replication = r;
}
inline void apply_trait(ComponentInfo& info, AsyncAware) noexcept
{
    info.async_aware = true;
}
inline void apply_trait(ComponentInfo& info, SpatialBVH) noexcept
{
    info.spatial_bvh = true;
}
inline void apply_trait(ComponentInfo& info, GpuResident) noexcept
{
    info.gpu_resident = true;
}
inline void apply_trait(ComponentInfo& info, History h) noexcept
{
    info.history_window = h.window;
}
inline void apply_trait(ComponentInfo& info, ComponentSerialize s) noexcept
{
    info.serialize = s;
}
inline void apply_trait(ComponentInfo& info, Reflection r) noexcept
{
    info.reflection = r;
}

// Relation traits (ADR-0051). Set by `register_relation<Tag>(...)` which
// forwards to `register_type<Relation<Tag>>(traits...)`. Components that
// aren't relations leave these flags at their defaults.
inline void apply_trait(ComponentInfo& info, ReverseIndex) noexcept
{
    info.has_reverse_index = true;
}
inline void apply_trait(ComponentInfo& info, Acyclic) noexcept
{
    info.acyclic = true;
}
inline void apply_trait(ComponentInfo& info, OnTargetDestroyed otd) noexcept
{
    info.has_on_target_destroyed = true;
    info.on_target_destroyed_policy = static_cast<crd::u8>(otd.policy);
}

// is_relation_instance_v<T> — true when T is a Relation<Tag> instantiation.
// Used by register_type to stamp ComponentInfo::is_relation = true.
template <typename T> struct IsRelationInstance : std::false_type
{
};
template <typename Tag> struct IsRelationInstance<Relation<Tag>> : std::true_type
{
};
template <typename T> inline constexpr bool is_relation_instance_v = IsRelationInstance<T>::value;

template <typename T> void capture_lifecycle_ops(ComponentInfo& info) noexcept
{
    if constexpr (std::is_default_constructible_v<T>)
    {
        info.default_construct = [](void* dst)
        {
            ::new (dst) T();
        };
    }
    if constexpr (std::is_destructible_v<T>)
    {
        info.destruct = [](void* p)
        {
            static_cast<T*>(p)->~T();
        };
    }
    if constexpr (std::is_move_constructible_v<T>)
    {
        info.move_construct = [](void* dst, void* src)
        {
            ::new (dst) T(std::move(*static_cast<T*>(src)));
        };
    }
}
} // namespace detail

// ComponentRegistry — table of registered component metadata, keyed two ways:
//   1. ComponentId → ComponentInfo (dense Array, monotonic from registration)
//   2. type-tag pointer → ComponentId (HashMap, for register-once-and-lookup)
//
// Owned by World. Construction reserves slot 0 (ComponentId::null) so id-of-T
// returns null for unregistered T regardless of how the lookup is encoded.
//
// Re-registration is idempotent: registering the same T twice returns the
// existing ComponentId and ignores the second call's trait arguments. This
// lets libraries register defensively without coordination.
//
// kMaxComponents (256) is enforced via CRD_ASSERT in register_type.
class ComponentRegistry
{
public:
    explicit ComponentRegistry(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;
    ComponentRegistry(ComponentRegistry&&) = default;
    ComponentRegistry& operator=(ComponentRegistry&&) = default;

    template <typename T, typename... Traits> ComponentId register_type(Traits&&... traits)
    {
        static_assert(!std::is_reference_v<T>, "ComponentRegistry::register_type<T>: T must not be a reference");
        const void* key = component_type_key<T>();
        if (auto* existing = m_id_by_key.find(key); existing != nullptr)
        {
            return *existing;
        }

        const auto raw_id = static_cast<crd::u16>(m_infos.size());
        // Always-on trap: exceeding kMaxComponents would silently corrupt
        // ComponentMask::set/test on the next registration (out-of-bounds
        // word index on the std::array<u64, 4>). Use CRD_FATAL so release
        // builds halt at registration rather than corrupt later.
        if (raw_id >= static_cast<crd::u16>(kMaxComponents))
        {
            CRD_FATAL("ComponentRegistry: kMaxComponents (256) exceeded");
        }

        ComponentInfo info{};
        info.id = ComponentId{raw_id};
        info.name = crd::containers::StringView{typeid(T).name()};
        info.size = sizeof(T);
        info.alignment = alignof(T);
        // Mark Relation<Tag> instantiations so the World relation API can
        // distinguish them from plain components without a separate table.
        if constexpr (detail::is_relation_instance_v<T>)
        {
            info.is_relation = true;
        }

        // Apply traits in argument order. Later traits override earlier ones
        // for fields that overlap (e.g. two StorageHints — last one wins).
        (detail::apply_trait(info, std::forward<Traits>(traits)), ...);

        detail::capture_lifecycle_ops<T>(info);

        m_infos.push_back(info);
        m_id_by_key.emplace(key, info.id);
        return info.id;
    }

    [[nodiscard]] const ComponentInfo* info(ComponentId id) const noexcept
    {
        if (id.is_null() || id.raw >= m_infos.size())
        {
            return nullptr;
        }
        return &m_infos[id.raw];
    }

    [[nodiscard]] crd::u16 size() const noexcept { return static_cast<crd::u16>(m_infos.size()); }

    template <typename T> [[nodiscard]] ComponentId id_of() const noexcept
    {
        const void* key = component_type_key<T>();
        if (const auto* found = m_id_by_key.find(key); found != nullptr)
        {
            return *found;
        }
        return ComponentId{};
    }

private:
    crd::containers::Array<ComponentInfo> m_infos;
    crd::containers::HashMap<const void*, ComponentId> m_id_by_key;
};

} // namespace crd::scene
