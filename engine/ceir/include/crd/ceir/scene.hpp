#pragma once

// crd-ceir — the SCENE dialect's resolver-handle TYPE-CLASSES + the find_scene_misuse verifier (CEIR-17a, §45). The four
// scene.resolve_* ops (generated from scene.ceirop.toml) each PRODUCE an opaque host handle — a material / technique /
// program / geometry — and consume the right one (the chain material→technique→program). Following the RENDER attachment
// precedent, each handle is an 8a Extern TYPE-CLASS (the ROLE rides the TYPE, per the 12a one-source-of-truth doctrine),
// so resolve_technique's operand being material-typed is a TYPE property the verifier checks — not a runtime cast. The
// handles are OPAQUE (zero-member Externs): CEIR sees an identity, never the ECS/host payload (the currency rule). Two
// different classes with identical (zero) params are DIFFERENT TypeIds (the ADR-0111 landmine) — draw != material != …
// The OPS are generated (register_scene_ops); this header adds the type-classes + factories + the combined
// register_dialect + the SEMANTIC type-chain walk (find_scene_misuse — its OWN misuse enum, parallel to
// find_render_misuse; the generated per-op verifier owns STRUCTURAL conformance, this owns the type/vocab chain).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/scene_ops.hpp> // register_scene_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::scene
{
// Register the `scene` dialect: its generated ops (register_scene_ops) + the five opaque resolver-handle type-classes.
// Idempotent. ⛔ Callers use THIS (not the raw generated register_scene_ops) so the handle types exist for the resolve
// ops' result types + find_scene_misuse.
Dialect* register_dialect(Context& ctx);

// The interned resolver-handle type-classes = intern_type_class("scene", "<name>"). Each a DISTINCT opaque 8a Extern
// class (draw != material != technique != program != geometry). Work for a not-yet-registered context (content-hash id);
// register_dialect gives them their verify hook.
[[nodiscard]] TypeClassId draw_class(Context& ctx);
[[nodiscard]] TypeClassId material_class(Context& ctx);
[[nodiscard]] TypeClassId technique_class(Context& ctx);
[[nodiscard]] TypeClassId program_class(Context& ctx);
[[nodiscard]] TypeClassId geometry_class(Context& ctx);

// Build an OPAQUE resolver-handle TYPE (a zero-member 8a Extern of the given class — the handle is host-opaque, no
// CEIR-visible payload). The class must be registered (type_extern asserts its hook).
[[nodiscard]] TypeId type_draw(Context& ctx);
[[nodiscard]] TypeId type_material(Context& ctx);
[[nodiscard]] TypeId type_technique(Context& ctx);
[[nodiscard]] TypeId type_program(Context& ctx);
[[nodiscard]] TypeId type_geometry(Context& ctx);

// ── the SCENE misuse walk — the SEMANTIC type-chain check (find_render_misuse's parallel). scene's OWN enum (NOT a
// widen of RenderMisuseKind — so no -Werror=switch audit ripples into the render verifier). Append at end. ──
enum class SceneMisuseKind : u8
{
    None = 0,
    DrawTypeMismatch,      // an operand that must be scene.draw isn't (resolve_material/geometry operand(0); resolve_program operand(1))
    MaterialTypeMismatch,  // scene.resolve_technique's operand(0) is not scene.material-typed
    TechniqueTypeMismatch, // scene.resolve_program's operand(0) is not scene.technique-typed
    PhaseInvalid,          // scene.resolve_technique's `phase` is not in {opaque, transparent, shadow, depth, velocity} (or non-String)
};
[[nodiscard]] containers::StringView scene_misuse_kind_name(SceneMisuseKind k) noexcept;

// The pointing result of the scene type-chain walk: the FIRST misuse (pre-order), the offending `op`, and the `value` it
// points at (the bad operand; null for an attribute misuse).
struct SceneMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    SceneMisuseKind  kind  = SceneMisuseKind::None;
};
// The FIRST scene misuse in module `m` (pre-order), or {None}. A resolve op's operand must be the right scene type-class
// (draw / material / technique); a resolve_technique's `phase` must be in its closed vocabulary. STRUCTURAL conformance
// (operand/result counts, phase-attr presence) is the generated per-op verifier — this is the type/vocab chain.
// (Takes Context& — NOT const — because the type-chain check must intern the operand class-ids; intern_type_class is
// non-const. The interning is a benign caching side-effect, done once at entry; the recursive walk itself is const.)
[[nodiscard]] SceneMisuse find_scene_misuse(Context& ctx, const Module& m);
} // namespace crd::ceir::scene
