#pragma once

// crd-ceir — the RT dialect's acceleration-structure/SBT TYPE-CLASSES + the find_rt_misuse verifier (CEIR-19a, §49). The
// six rt ops (generated from rt.ceirop.toml) orchestrate ray tracing: blas_build → instance_populate → tlas_build →
// sbt_build → trace (the pipeline path) or ray_query (the inline path). Following the SCENE / RENDER attachment precedent,
// the three device handles are 8a Extern TYPE-CLASSES (the ROLE rides the TYPE, per the 12a one-source-of-truth doctrine):
// rt.blas / rt.tlas / rt.sbt. ⛔ DISTINCT TypeIds are LOAD-BEARING — rt.trace consumes a %tlas + a %sbt while rt.ray_query
// consumes a %tlas ONLY (the inline-vs-pipeline distinction is verifiable only if tlas != sbt). The handles are OPAQUE
// (zero-member Externs): CEIR sees an identity, never the driver's VkAccelerationStructureKHR (the currency rule; the
// engine's fused RtScene backs %blas + %tlas — the 17b shared-handle precedent). Two classes with identical (zero) params
// are DIFFERENT TypeIds (the ADR-0111 landmine) — blas != tlas != sbt. The OPS are generated (register_rt_ops); this
// header adds the type-classes + factories + register_dialect + the SEMANTIC type-chain walk (find_rt_misuse — its OWN
// misuse enum, parallel to find_scene_misuse; the generated per-op verifier owns STRUCTURAL conformance, this owns the
// type/vocab chain).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/rt_ops.hpp> // register_rt_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::rt
{
// Register the `rt` dialect: its generated ops (register_rt_ops) + the three opaque AS/SBT-handle type-classes. Idempotent.
// ⛔ Callers use THIS (not the raw generated register_rt_ops) so the handle types exist for the ops' result types +
// find_rt_misuse.
Dialect* register_dialect(Context& ctx);

// The interned handle type-classes = intern_type_class("rt", "<name>"). Each a DISTINCT opaque 8a Extern class
// (blas != tlas != sbt). Work for a not-yet-registered context (content-hash id); register_dialect gives them their hook.
[[nodiscard]] TypeClassId blas_class(Context& ctx);
[[nodiscard]] TypeClassId tlas_class(Context& ctx);
[[nodiscard]] TypeClassId sbt_class(Context& ctx);

// Build an OPAQUE handle TYPE (a zero-member 8a Extern of the given class — host-opaque, no CEIR-visible payload). The
// class must be registered (type_extern asserts its hook).
[[nodiscard]] TypeId type_blas(Context& ctx);
[[nodiscard]] TypeId type_tlas(Context& ctx);
[[nodiscard]] TypeId type_sbt(Context& ctx);

// ── the RT misuse walk — the SEMANTIC type-chain check (find_scene_misuse's parallel). rt's OWN enum (NOT a widen of
// SceneMisuseKind/RenderMisuseKind — so no -Werror=switch audit ripples). Append at end. ──
enum class RtMisuseKind : u8
{
    None = 0,
    BlasTypeMismatch,     // rt.instance_populate's operand(0) is not rt.blas-typed
    TlasTypeMismatch,     // rt.trace's / rt.ray_query's operand(3) is not rt.tlas-typed
    SbtTypeMismatch,      // rt.trace's operand(4) is not rt.sbt-typed (the pipeline path needs an SBT; ray_query must NOT bind one)
    InstanceCountInvalid, // rt.instance_populate's `instance_count` is < 1 (or non-Int)
    GeometryKindInvalid,  // rt.blas_build's `geometry_kind` is not in {triangles, procedural, cluster}
    MaxRecursionInvalid,  // rt.trace's `max_recursion`, if present, is < 1 (or non-Int)
    // ── the dispatch-shape checks (the find_dispatch_misuse mirror — trace/ray_query are compute.dispatch's RT siblings:
    //    a launch grid + variadic bindings + a per-binding `access` string). ⛔ ORDER is contractual (negatives pin the exact
    //    kind): tlas/sbt type-class → dims-index → access(kind-fold → tokens → arity) → bindings-resource → max_recursion. ──
    DimNotIndex,          // a launch-dim operand (trace/ray_query operands 0-2) is not Index-typed
    AccessTokenInvalid,   // `access` is non-String, or a token is not exactly one of {r, w, rw} (the 12b wrong-kind fold)
    AccessArityMismatch,  // the `access` token count != the number of variadic bindings
    BindingNotResource,   // a variadic binding operand (trace 5.., ray_query 4..) is not a resource-kinded value
};
[[nodiscard]] containers::StringView rt_misuse_kind_name(RtMisuseKind k) noexcept;

// The pointing result of the rt type-chain walk: the FIRST misuse (pre-order), the offending `op`, and the `value` it
// points at (the bad operand; null for an attribute misuse).
struct RtMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    RtMisuseKind     kind  = RtMisuseKind::None;
};
// The FIRST rt misuse in module `m` (pre-order), or {None}. This owns the TYPE/VOCAB CHAIN: an op's operand is the right rt
// type-class (instance_populate blas / trace|ray_query tlas / trace sbt); blas_build's `geometry_kind` in its closed vocabulary;
// instance_count / max_recursion >= 1; AND the dispatch-shape operand types for trace/ray_query (the find_dispatch_misuse
// mirror — dims Index-typed, the `access` tokens {r|w|rw} and their arity == the binding count, bindings resource-kinded).
// STRUCTURAL conformance (operand/result/region counts, required-attr PRESENCE + KIND — `access` String, `kernel`/`raygen`
// SymbolRef, `instance_count` Int) is the GENERATED per-op verifier (verify_* in rt_ops.cpp). (Takes Context& — NOT const — because the check interns the operand class-ids;
// intern_type_class is non-const. The interning is a benign caching side-effect done once at entry; the walk itself is const.)
[[nodiscard]] RtMisuse find_rt_misuse(Context& ctx, const Module& m);
} // namespace crd::ceir::rt
