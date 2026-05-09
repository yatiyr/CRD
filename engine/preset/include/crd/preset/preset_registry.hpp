#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/preset/preset_loader.hpp>

#include <memory>

namespace crd::preset
{
// Per-type metadata in the registry. Closed-by-types via register_type<T>().
struct PresetTypeInfo
{
    crd::u32                fourcc{};
    crd::u32                latest_schema_version{};
    crd::u32                size_bytes{};
    crd::u32                alignment{};
    crd::containers::String name;       // e.g. "Quality", "Camera"; diagnostic only
    PresetLoader*           loader = nullptr; // owned by the registry; non-owning view here
};

// PresetRegistry — closed-by-C++-types registry of preset schemas.
//
// register_type<T>() creates and owns a per-type PresetLoader instance,
// keyed on T::fourcc. Idempotent: a second register_type<T>() returns the
// existing entry without re-registering or replacing the loader (matches
// the ADR-0053 ComponentRegistry idempotency contract — libraries can
// register defensively without coordination).
//
// The schema struct contract for T is:
//
//   struct MyPresetSchema
//   {
//       static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P','R','M','Y');
//       static constexpr crd::u32 version = 1U;
//       // ... fields with default values, in canonical declaration order
//   };
//
// `T::fourcc` and `T::version` must be `static constexpr crd::u32`. The
// registry asserts `static_assert(sizeof(T) <= UINT32_MAX)` at compile
// time (effectively trivially satisfied; presets are small structs).
class PresetRegistry
{
public:
    explicit PresetRegistry(crd::memory::IAllocator* alloc);

    PresetRegistry(const PresetRegistry&)            = delete;
    PresetRegistry& operator=(const PresetRegistry&) = delete;
    PresetRegistry(PresetRegistry&&)                 = delete;
    PresetRegistry& operator=(PresetRegistry&&)      = delete;
    ~PresetRegistry()                                = default;

    // Register T's schema. Returns the registered (or existing on re-register)
    // PresetTypeInfo. `name` is ignored on re-registration.
    template <typename T>
    [[nodiscard]] const PresetTypeInfo& register_type(crd::containers::StringView name);

    // Lookup by FourCC; nullptr if not registered.
    [[nodiscard]] const PresetTypeInfo* find(crd::u32 fourcc) const noexcept;

    // Lookup by name; O(N) — used at TOML cooker time, never on the hot path.
    [[nodiscard]] const PresetTypeInfo* find(crd::containers::StringView name) const noexcept;

    [[nodiscard]] crd::usize size() const noexcept { return m_types.size(); }

    [[nodiscard]] crd::containers::ConstSpan<PresetTypeInfo> types() const noexcept
    {
        return crd::containers::ConstSpan<PresetTypeInfo>{m_types.data(), m_types.size()};
    }

private:
    [[nodiscard]] PresetTypeInfo& register_raw(crd::u32 fourcc,
                                               crd::u32 schema_version,
                                               crd::u32 size_bytes,
                                               crd::u32 alignment,
                                               crd::containers::StringView name);

    crd::memory::IAllocator*                          m_alloc;
    crd::containers::Array<PresetTypeInfo>            m_types;
    crd::containers::HashMap<crd::u32, crd::u32>      m_by_fourcc; // fourcc → index in m_types
    crd::containers::Array<std::unique_ptr<PresetLoader>> m_owned_loaders;
};

template <typename T>
const PresetTypeInfo& PresetRegistry::register_type(crd::containers::StringView name)
{
    static_assert(static_cast<crd::u32>(T::fourcc) != 0U,
                  "Preset schema struct must declare `static constexpr crd::u32 fourcc`");
    static_assert(static_cast<crd::u32>(T::version) >= 1U,
                  "Preset schema version must be >= 1");
    static_assert(sizeof(T)  <= 0xFFFFFFFFULL, "Preset schema must fit in u32 size");
    static_assert(alignof(T) <= 0xFFFFFFFFULL, "Preset schema alignment must fit in u32");

    return register_raw(T::fourcc,
                        T::version,
                        static_cast<crd::u32>(sizeof(T)),
                        static_cast<crd::u32>(alignof(T)),
                        name);
}

} // namespace crd::preset
