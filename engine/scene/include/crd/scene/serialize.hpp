#pragma once

#include <crd/core/types.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/relation.hpp>

namespace crd::scene
{
// Phase 3.0 v1k — SCEN serialisation FourCC table (ADR-0055).
//
// Every component or relation that wants to round-trip through SCEN
// must be registered with a `ComponentSerialize{fourcc, version, ...}`
// trait. The FourCC is the SOURCE OF TRUTH for type identity at load
// time; the type's textual name (typeid().name()) is stored in the
// SCEN string pool for diagnostic/log purposes only.
//
// FourCC convention:
//   - Components: 4-letter mnemonic, e.g. 'XFRM' for Transform.
//   - Built-in relations: 'R' prefix + 3-letter abbreviation, e.g.
//     'RChO' for Relation<ChildOf>.
//   - User-defined: choose any 4-byte tag; reserved space is 0x00–0x1F
//     bytes (control chars). FourCCs starting with 'R' are reserved
//     for relations.
//
// Determinism: this table fixes the FourCCs at v1k. Future builds must
// not change a built-in's FourCC — that would invalidate every saved
// SCEN. New built-ins get new unique FourCCs.

inline constexpr crd::u32 make_serialize_fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<crd::u32>(static_cast<unsigned char>(a))
         | (static_cast<crd::u32>(static_cast<unsigned char>(b)) << 8U)
         | (static_cast<crd::u32>(static_cast<unsigned char>(c)) << 16U)
         | (static_cast<crd::u32>(static_cast<unsigned char>(d)) << 24U);
}

// Built-in component FourCCs (Phase 3.0 v1k+).
inline constexpr crd::u32 kFourCC_Transform = make_serialize_fourcc('X', 'F', 'R', 'M');

// Built-in relation FourCCs (Phase 3.0 v1k+).
inline constexpr crd::u32 kFourCC_RelChildOf     = make_serialize_fourcc('R', 'C', 'h', 'O');
inline constexpr crd::u32 kFourCC_RelAttachedTo  = make_serialize_fourcc('R', 'A', 't', 'T');
inline constexpr crd::u32 kFourCC_RelOwns        = make_serialize_fourcc('R', 'O', 'w', 'n');
inline constexpr crd::u32 kFourCC_RelTargets     = make_serialize_fourcc('R', 'T', 'g', 't');
inline constexpr crd::u32 kFourCC_RelDependsOn   = make_serialize_fourcc('R', 'D', 'e', 'p');
inline constexpr crd::u32 kFourCC_RelPossessedBy = make_serialize_fourcc('R', 'P', 'o', 's');

// Convenience: build a default ComponentSerialize trait for a POD
// (trivially-copyable) type. Reads/writes default to nullptr → the
// SCEN loader falls back to memcpy of `info->size` bytes. This is the
// right default for Transform, the relations, and all built-in PODs.
//
// Usage:
//   world.register_component<MyComponent>(default_serialize_trait<MyComponent>('MYTC', 1));
template <typename T>
[[nodiscard]] constexpr ComponentSerialize default_serialize_trait(crd::u32 fourcc, crd::u32 version = 1) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "default_serialize_trait<T>: T must be trivially copyable. "
                  "Non-trivial types must supply explicit read_blob/write_blob callbacks.");
    ComponentSerialize cs{};
    cs.fourcc          = fourcc;
    cs.version         = version;
    // read_blob / write_blob remain nullptr — loader uses memcpy fallback.
    return cs;
}

// Built-in serialize traits. Helpers so the user can opt into per-built-in
// serialisation by calling `world.register_serialize_traits_for_builtins()`
// (a single call that adds the traits to Transform + the six built-in
// relations). v1k tests use this; v1l cooker will too.
[[nodiscard]] inline ComponentSerialize transform_serialize_trait() noexcept
{
    // Transform contains f32 + Quat + f32 + Mat4 — all trivially copyable.
    // memcpy round-trips correctly.
    ComponentSerialize cs{};
    cs.fourcc  = kFourCC_Transform;
    cs.version = 1;
    return cs;
}

[[nodiscard]] inline ComponentSerialize relation_serialize_trait(crd::u32 fourcc) noexcept
{
    // Relation<Tag> is layout-equivalent to a single EntityId (8 bytes).
    // memcpy round-trips correctly.
    ComponentSerialize cs{};
    cs.fourcc  = fourcc;
    cs.version = 1;
    return cs;
}

// SCEN artifact format constants.
inline constexpr crd::u32 kSceneSchemaVersion = 1U;
// FourCC for the SCEN container (CRDR type_fourcc).
inline constexpr crd::u32 kFourCC_SCEN = make_serialize_fourcc('S', 'C', 'E', 'N');
// SCEN-internal chunk FourCCs.
inline constexpr crd::u32 kFourCC_SceneINFO = make_serialize_fourcc('I', 'N', 'F', 'O');
inline constexpr crd::u32 kFourCC_SceneSTRP = make_serialize_fourcc('S', 'T', 'R', 'P');
inline constexpr crd::u32 kFourCC_SceneCMPS = make_serialize_fourcc('C', 'M', 'P', 'S');
inline constexpr crd::u32 kFourCC_SceneETBL = make_serialize_fourcc('E', 'T', 'B', 'L');
inline constexpr crd::u32 kFourCC_SceneRELS = make_serialize_fourcc('R', 'E', 'L', 'S');

// ---- Öbek (Phase 3.0 v1m, ADR-0058) ----
//
// Öbek = a cooked, instantiable cluster of entities + components +
// relations. CRDR container with type_fourcc = 'OBEK'. Chunks distinct
// from SCEN's even though both share the per-component 'C###' payload
// shape — separate FourCCs make tooling diagnostics unambiguous.
//
// v1m1 ships the substrate chunks (OINF / OETB / OCMP / ORLS).
// v1m2 adds OOVR (overrides) + OCHN (extends/nested chain).
// v1m5 reserves OBAT (batch hints) + OLNK (lazy refs) at the format
// level for Phase 3.5+ runtime backends.
inline constexpr crd::u32 kObekSchemaVersion = 1U;
inline constexpr crd::u32 kFourCC_OBEK     = make_serialize_fourcc('O', 'B', 'E', 'K');
inline constexpr crd::u32 kFourCC_ObekOINF = make_serialize_fourcc('O', 'I', 'N', 'F');
inline constexpr crd::u32 kFourCC_ObekOETB = make_serialize_fourcc('O', 'E', 'T', 'B');
inline constexpr crd::u32 kFourCC_ObekOCMP = make_serialize_fourcc('O', 'C', 'M', 'P');
inline constexpr crd::u32 kFourCC_ObekORLS = make_serialize_fourcc('O', 'R', 'L', 'S');
inline constexpr crd::u32 kFourCC_ObekSTRP = make_serialize_fourcc('O', 'S', 'T', 'R');
// Reserved for v1m2+:
inline constexpr crd::u32 kFourCC_ObekOOVR = make_serialize_fourcc('O', 'O', 'V', 'R');
inline constexpr crd::u32 kFourCC_ObekOCHN = make_serialize_fourcc('O', 'C', 'H', 'N');
// Reserved for v1m5 / Phase 3.5+ runtime:
inline constexpr crd::u32 kFourCC_ObekOBAT = make_serialize_fourcc('O', 'B', 'A', 'T');
inline constexpr crd::u32 kFourCC_ObekOLNK = make_serialize_fourcc('O', 'L', 'N', 'K');

// Per-component payload chunk FourCC inside an OBEK container — uses the
// 'D000'-'D0FF' range to be unambiguous from SCEN's 'C000'-'C0FF'. Same
// SoA layout (record_count + indices[] + (pad) + payloads[]).
[[nodiscard]] inline constexpr crd::u32 make_obek_component_chunk_fourcc(crd::u32 file_local_id) noexcept
{
    auto hex = [](crd::u32 nibble) constexpr -> char
    {
        return (nibble < 10U) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10U));
    };
    const char d2 = hex((file_local_id >> 8U) & 0xFU);
    const char d1 = hex((file_local_id >> 4U) & 0xFU);
    const char d0 = hex(file_local_id & 0xFU);
    return make_serialize_fourcc('D', d2, d1, d0);
}

// Build the FourCC for the per-component-id payload chunk. file_local_id
// is in [0, 256) — emitted as 3 hex chars after the 'C' prefix.
//   id=0   → 'C000'
//   id=15  → 'C00F'
//   id=255 → 'C0FF'
[[nodiscard]] inline constexpr crd::u32 make_scene_component_chunk_fourcc(crd::u32 file_local_id) noexcept
{
    auto hex = [](crd::u32 nibble) constexpr -> char
    {
        return (nibble < 10U) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10U));
    };
    const char d2 = hex((file_local_id >> 8U) & 0xFU);
    const char d1 = hex((file_local_id >> 4U) & 0xFU);
    const char d0 = hex(file_local_id & 0xFU);
    return make_serialize_fourcc('C', d2, d1, d0);
}

} // namespace crd::scene
