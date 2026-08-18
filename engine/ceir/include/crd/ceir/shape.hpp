#pragma once

// crd-ceir — the shape dialect's find_shape_misuse verifier (CEIR-21a, §35). Six PURE value-ops (generated from
// shape.ceirop.toml) over the CEIR-3d shape TYPES (Dim/Shape, already TypeKinds since 3d): make / rank / extent / broadcast /
// reshape / assert. ⛔ NO new type-classes (unlike work/rt — Dim/Shape EXIST; this dialect operates OVER them, mints none), so
// register_dialect just registers the generated ops. The generated per-op verifier owns STRUCTURAL conformance (operand/result
// counts + required-attr PRESENCE + KIND — `axis` Int, `relation` String); THIS owns the TYPE + SEMANTIC chain: operand/result
// TypeKinds (Shape/Dim/Index) + the 3d tri-state predicates (shapes_broadcast → BroadcastResult, shapes_reshape → ShapeCompat)
// with the band-locked contract — Incompatible → a POINTING misuse (the 3z BroadcastResult.position), Unknown → ACCEPT (the
// principled deferral a shape.assert discharges at runtime), Compatible → accept. ⛔ I6 — find_shape_misuse switches on op NAME,
// never op.kind. ⛔ Declare-only: the ops carry typed NoSemantics + NO kernel_ref / NO lowering hook (§70: a NATIVE graph backend
// must ingest the tensor+shape graph WHOLE — never flatten a shape query to a dispatch).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/shape_ops.hpp> // register_shape_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::shape
{
// Register the `shape` dialect: its generated ops (register_shape_ops). Idempotent. ⛔ NO type-classes — Dim/Shape are 3d
// TypeKinds this dialect operates over, not new Externs.
Dialect* register_dialect(Context& ctx);

// ── the shape misuse walk — the SEMANTIC TYPE + PREDICATE chain (find_work_misuse's parallel; shape's OWN enum). Append at
//    end. STRUCTURAL conformance (arity, `axis`/`relation` presence+kind) is the GENERATED per-op verifier (verify_* in
//    shape_ops.cpp); this owns operand/result TypeKinds + the 3d shape-relation predicates. ──
enum class ShapeMisuseKind : u8
{
    None = 0,
    OperandNotShape,            // a Shape operand (rank/extent op0; broadcast/reshape/assert op0+op1) is not Shape-typed
    MakeOperandNotDim,          // a shape.make variadic operand is not Dim-typed
    ResultNotShape,             // a shape-producing op's result (make/broadcast/reshape/assert) is not Shape-typed
    RankResultNotIndex,         // shape.rank's result is not Index-typed
    ExtentResultNotDim,         // shape.extent's result is not Dim-typed
    ExtentAxisInvalid,          // shape.extent's `axis` < 0 or >= the operand shape's rank (members.size(); rank is always static)
    ShapeBroadcastIncompatible, // shape.broadcast: shapes_broadcast(lhs,rhs) == Incompatible (position = the right-aligned axis)
    ShapeReshapeIncompatible,   // shape.reshape: shapes_reshape(src,target) == Incompatible
    AssertRelationInvalid,      // shape.assert's `relation` not in the closed vocab {equal,broadcast,reshape}
    AssertResultMismatch,       // shape.assert's result type != its lhs operand type (it passes lhs through)
    // ── RESULT-IDENTITY checks (the 12a `underlying == operand` precedent — the declare-slice verifier enforces the TOML's
    //    declared result IDENTITY, not merely its KIND; a result whose TYPE prints Shape/Dim but is the WRONG shape/dim is a
    //    distinct misuse). ⛔ broadcast's result-exactness needs shapes_broadcast_result — name-forwarded to 21b (its elementwise
    //    consumer), see the shape.broadcast TOML docs; here broadcast only checks compat + result-KIND. ──
    MakeResultShapeMismatch,    // shape.make's result Shape's members != the operand dims (rank or a per-axis dim differs)
    ExtentResultMismatch,       // shape.extent's result Dim != the operand shape's member at `axis`
    ReshapeResultMismatch,      // shape.reshape's result != the `target` operand's type (it passes the validated target through)
};
[[nodiscard]] containers::StringView shape_misuse_kind_name(ShapeMisuseKind k) noexcept;

// The pointing result of the shape type-chain walk: the FIRST misuse (pre-order), the offending `op`, the `value` it points at
// (the bad operand/result; null for an attribute misuse), and `position` = the RIGHT-ALIGNED axis for ShapeBroadcastIncompatible
// (else -1) — the 3z pointing-diagnostic contract (the broadcast diag names the exact bad pair).
struct ShapeMisuse
{
    const Value*     value    = nullptr;
    const Operation* op       = nullptr;
    ShapeMisuseKind  kind     = ShapeMisuseKind::None;
    i32              position = -1;
};
// The FIRST shape misuse in module `m` (pre-order), or {None}. Owns the TYPE/PREDICATE CHAIN (see the enum). const Context&
// (no interning — unlike work: shape mints no type-classes; the 3d predicates + type_of + attr_value are all const).
[[nodiscard]] ShapeMisuse find_shape_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::shape
