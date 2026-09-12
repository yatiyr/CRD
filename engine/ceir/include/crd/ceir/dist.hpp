#pragma once

// crd-ceir — the dist dialect's find_dist_misuse verifier (CEIR-30a-1, sec-68/sec-103; proof sec-140). The GENERATED
// verify_mesh/verify_shard/verify_all_reduce (gen/dist_ops.hpp) own each op's STRUCTURAL contract (operand/result counts +
// required-attr PRESENCE + KIND — `name`/`mesh` Symbol, `shape`/`fn` String, `axis`/`mesh_axis` Int); THIS owns the
// SEMANTIC + MODULE-WIDE rules: a mesh `shape` must be >=1 positive ints (MeshShapeInvalid); a duplicate mesh `name` is
// DuplicateMesh; a shard/all_reduce `mesh` must RESOLVE to a dist.mesh in the module (UnknownMesh); their operand+result
// must be Tensor-kinded (OperandNotTensor) and the result type must EQUAL the input type (ResultTypeMismatch — the sec-70
// don't-prematurely-lower invariant: a placement annotation PRESERVES the logical tensor); a shard `axis` is bounded by
// the input rank (ShardAxisInvalid) and its `mesh_axis` by the mesh rank (MeshAxisInvalid); an all_reduce `fn` is in
// {sum,prod,max,min} (FnInvalid — tensor.reduce's vocab minus mean, the 30a-3 post-scale named-forward). The find_resource/
// tensor/transform_misuse house pattern. ⛔ I6 — matches op NAME (ctx.op_name, const), never op.kind. ⛔ the mesh NAME is
// resolved by THIS walk's own module scan (attr-only name-match — the frame.draw-list-name precedent, NOT a global
// SymbolTable entry this slice; a SymbolTable exists since CEIR-13 but dist.mesh is not registered into it here). ⛔ const
// Context& — reads names/attrs/types and COMPARES TypeIds, interns NOTHING (unlike find_tensor_misuse).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/dist_ops.hpp> // register_dist_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::dist
{
enum class DistMisuseKind : u8
{
    None = 0,
    MeshShapeInvalid,   // dist.mesh `shape` is not >=1 POSITIVE ints (bad token / a zero or negative dim / empty)
    DuplicateMesh,      // two dist.mesh with the same `name` (the SECOND occurrence, pre-order)
    UnknownMesh,        // a dist.shard/dist.all_reduce `mesh` resolves to NO dist.mesh in the module (or is non-Symbol)
    OperandNotTensor,   // a dist.shard/dist.all_reduce operand OR result is not Tensor-kinded
    ResultTypeMismatch, // a dist.shard/dist.all_reduce result type != input type (the placement annotation must preserve the logical tensor)
    ShardAxisInvalid,   // dist.shard `axis` < 0 or >= input tensor rank (or non-Int)
    MeshAxisInvalid,    // dist.shard `mesh_axis` < 0 or >= mesh rank (the resolved mesh's `shape` length) (or non-Int)
    FnInvalid,          // dist.all_reduce `fn` not in {sum,prod,max,min}
};
[[nodiscard]] containers::StringView dist_misuse_kind_name(DistMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, and the `value` it points at (the operand/result
// for a tensor/type misuse; null for an attr/mesh misuse — the tensor/quant misuse pointing convention).
struct DistMisuse
{
    const Value*   value = nullptr;
    const Operation* op  = nullptr;
    DistMisuseKind kind  = DistMisuseKind::None;
};
// The FIRST dist misuse in module `m` (pre-order), or {None}. ⛔ const Context& — reads op names/attrs/types, interns nothing.
[[nodiscard]] DistMisuse find_dist_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::dist
