#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <array>

namespace crd::scene
{
// ComponentId — 16-bit per-World identity for a registered component type.
//
// Allocated monotonically from registration order. Stable for the life of the
// World. The reserved value 0xFFFF is the null sentinel; default-constructed
// ComponentId is null.
struct ComponentId
{
    crd::u16 raw = 0xFFFF;

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0xFFFF; }

    [[nodiscard]] constexpr bool operator==(const ComponentId&) const noexcept = default;
};

// Maximum number of component types registrable in one World.
//
// 256 fits four u64 words on a single cache line. The chunk-iteration hot path
// (ADR-0050 §6, ADR-0053 §8) does mask-AND across registered indexes; four-word
// AND is one cache line and ~four cycles, indistinguishable from one-word AND
// at the dispatch granularity (per-chunk-batch, not per-entity).
inline constexpr crd::u32 kMaxComponents = 256;

// 256-bit fixed bitset over ComponentIds. Storage layer (v1c) and index dispatch
// (v1i) consume this for archetype identity and observer matching.
struct ComponentMask
{
    std::array<crd::u64, 4> bits{};

    constexpr void set(ComponentId id) noexcept
    {
        const auto i = static_cast<crd::u32>(id.raw);
        bits[i >> 6] |= (crd::u64{1} << (i & 0x3F));
    }

    constexpr void clear(ComponentId id) noexcept
    {
        const auto i = static_cast<crd::u32>(id.raw);
        bits[i >> 6] &= ~(crd::u64{1} << (i & 0x3F));
    }

    [[nodiscard]] constexpr bool test(ComponentId id) const noexcept
    {
        const auto i = static_cast<crd::u32>(id.raw);
        return (bits[i >> 6] & (crd::u64{1} << (i & 0x3F))) != 0;
    }

    [[nodiscard]] constexpr bool any() const noexcept { return (bits[0] | bits[1] | bits[2] | bits[3]) != 0; }

    [[nodiscard]] constexpr crd::u32 popcount() const noexcept
    {
        crd::u32 n = 0;
        for (crd::u64 w : bits)
        {
            // Manual popcount — std::popcount lives in <bit>; keep this header light.
            while (w != 0)
            {
                w &= (w - 1);
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] constexpr bool operator==(const ComponentMask&) const noexcept = default;

    constexpr ComponentMask& operator&=(const ComponentMask& o) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            bits[i] &= o.bits[i];
        }
        return *this;
    }

    constexpr ComponentMask& operator|=(const ComponentMask& o) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            bits[i] |= o.bits[i];
        }
        return *this;
    }

    [[nodiscard]] friend constexpr ComponentMask operator&(ComponentMask a, const ComponentMask& b) noexcept
    {
        a &= b;
        return a;
    }

    [[nodiscard]] friend constexpr ComponentMask operator|(ComponentMask a, const ComponentMask& b) noexcept
    {
        a |= b;
        return a;
    }
};

// Storage backend selection at registration. ADR-0050.
enum class StorageHint : crd::u8
{
    Archetype = 0, // default — chunked SoA, primary backend
    SparseSet,     // escape hatch — high-churn / sparse / lookup-dominated
};

// Replication policy. ADR-0056. Phase 3.0 stores; Phase 4.2 honours.
enum class Replication : crd::u8
{
    Local = 0,           // never replicated; default
    ServerAuthoritative, // server writes, clients read; sent on change
    ClientPredicted,     // client writes, server validates; rollback-capable
    Remote,              // read-only mirror of remote owner's state
};

// ---- Trait markers ------------------------------------------------------
// Empty / value structs accepted at registration. ADR-0053.

// Reserved index slots (Phase 3.0 stores the flag; impl in consumer phase).
struct AsyncAware
{
}; // ships in Phase 3.0 v1i
struct SpatialBVH
{
}; // impl Phase 3.5 (light culling at scale)
struct GpuResident
{
}; // impl Phase 3.8 (GPU-driven rendering)

// Ring-buffer-of-N-frames history per component. Default 0 = disabled.
struct History
{
    crd::u8 window = 0;
};

// ---- Reserved trait records --------------------------------------------
// Declared so register_component accepts them; runtime treats as opaque.

struct ComponentSerialize
{
    crd::u32 fourcc = 0;  // identifies the component-type chunk in SCEN
    crd::u32 version = 0; // schema version

    // Callbacks populated by component owner; runtime invokes them in v1k–v1l.
    void (*deserialize_toml)(void* dst, const void* toml_table) = nullptr;
    void (*serialize_toml)(const void* src, void* toml_table_out) = nullptr;
    void (*read_blob)(void* dst, const void* bytes, crd::usize size) = nullptr;
    void (*write_blob)(const void* src, void* bytes_out, crd::usize cap) = nullptr;
};

struct Reflection
{
    // v1b stub: editor (Phase 7) populates fields. Runtime never walks this.
    crd::containers::StringView display_name{};
    const void* fields = nullptr; // opaque field-record pointer
};

// ---- Type-erased lifecycle ops -----------------------------------------

using DefaultCtorFn = void (*)(void* dst);
using DtorFn = void (*)(void* p);
using MoveCtorFn = void (*)(void* dst, void* src);

// ---- ComponentInfo: per-registration metadata --------------------------

struct ComponentInfo
{
    ComponentId id;
    crd::containers::StringView name; // typeid().name() — static storage; do not allocate

    crd::usize size = 0;
    crd::usize alignment = 0;

    StorageHint storage_hint = StorageHint::Archetype;

    // Trait flags (cheap to set, cheap to test)
    bool async_aware = false;
    bool spatial_bvh = false;
    bool gpu_resident = false;
    crd::u8 history_window = 0;

    Replication replication = Replication::Local;

    // Reserved trait records — accepted from day one (ADR-0056).
    ComponentSerialize serialize{};
    Reflection reflection{};

    // ---- Relation flags (ADR-0051, Phase 3.0 v1f) ----------------------
    // These are populated when `register_relation<Tag>(...)` is called for
    // this component (since `Relation<Tag>` is registered as a component).
    // For non-relation components they remain at defaults (false / SetNull).
    //
    // The relation system in World reads these directly to drive reverse-
    // index maintenance, cycle detection, and on-destroy policy. Keeping
    // the flags in ComponentInfo (rather than a parallel table) matches
    // the existing trait-dispatch grammar and keeps lookups branch-free.

    bool is_relation              = false; // marks this component as a Relation<Tag> instance
    bool acyclic                  = false; // Acyclic{} trait
    bool has_reverse_index        = false; // ReverseIndex{} trait
    bool has_on_target_destroyed  = false; // OnTargetDestroyed{...} trait present
    crd::u8 on_target_destroyed_policy = 0;        // raw OnTargetDestroyed::Policy value

    // Type-erased lifecycle ops. Captured via if-constexpr at registration;
    // any op may be nullptr if the type does not provide it (storage layer
    // checks and CRD_ASSERTs at use time).
    DefaultCtorFn default_construct = nullptr;
    DtorFn destruct = nullptr;
    MoveCtorFn move_construct = nullptr;
};

} // namespace crd::scene
