#pragma once

// crd-ceir — the TYPED open-world op-INTERFACE surface (CEIR-8e, ADR-0115). Plugin BEHAVIOR lives here: an op-kind
// implements a typed function-table interface; an analysis dispatches through it, NEVER a `switch(op.kind)` (§7) and
// NEVER a new OpTrait bit (traits are the closed core reasoning axes — dialect.hpp). `InterfaceId` is an FNV of the
// interface name (like TypeClassId/AttrClassId/LocationClassId), so a typed interface's `kId` is a COMPILE-TIME
// constant and register/query are id+cast-safe (a caller cannot pair the wrong id with the wrong type).

#include <crd/ceir/context.hpp> // Context complete — the typed helpers call register_interface/get_interface
#include <crd/ceir/id.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
class Operation;

// CEIR-8g: uses the ONE shared `fnv1a_ct` (id.hpp) — byte-identical to `containers::fnv1a_64`, so `T::kId` matches the
// runtime `intern_interface(name)`. (The former local `interface_hash_ct` was hoisted to id.hpp so analysis + diagnostic
// ids share it and cannot drift.)
[[nodiscard]] constexpr InterfaceId make_interface_id(const char* name) noexcept { return InterfaceId{fnv1a_ct(name)}; }

// ⭐ The LIVE proof interface (ADR-0115 §2.2): "what does this op-kind cost?" An analysis dispatches through THIS,
// never a switch on op.kind. Cost is the one catalog family with no existing home (MemoryEffect/Shape/Lowering/… live
// in existing machinery — see reserved_interfaces); a dialect registers a CostInterface with ZERO central-enum edits.
struct CostInterface
{
    static constexpr const char* kName = "crd.iface.cost";
    static constexpr InterfaceId kId   = make_interface_id("crd.iface.cost");
    u64 (*cost)(const Context&, const Operation&) noexcept; // the op-kind's cost model (abstract units)
};

// The RESERVED canonical interface NAMES (ADR-0115 §2.2 catalog). ⛔ Each family's behavior ALREADY has a home; a
// future slice binds an impl to the SAME name against that home, never a fresh forked vtable (the anti-pattern).
namespace reserved_interfaces
{
inline constexpr const char* kMemoryEffect  = "crd.iface.memory_effect"; // → EffectRecord/EffectsFn (CEIR-4a/8c)
inline constexpr const char* kShape         = "crd.iface.shape";         // → opgen type_inference/shape_inference (3d)
inline constexpr const char* kLowering      = "crd.iface.lowering";      // → IExecutionProvider seam (ADR-0109 §69)
inline constexpr const char* kTimeline      = "crd.iface.timeline";      // → region_exec attr (CEIR-4c) + 8f time
inline constexpr const char* kIncremental   = "crd.iface.incremental";   // → CEIR-8h incremental unification
inline constexpr const char* kConstraint    = "crd.iface.constraint";    // → 8c Constraint family + constraint dialect
inline constexpr const char* kSerialization = "crd.iface.serialization"; // → 8a/8b/8c class serialize + 8d STID
} // namespace reserved_interfaces

// Typed register/query — the id + the cast are internal, so a caller can never pair the wrong id with the wrong type.
// A dialect adds a capability by registering a `T` impl on an op-kind; an analysis queries it type-safely. `T` must
// carry `static constexpr InterfaceId kId`. The `impl` outlives the Context (a static function table — the OpInfo
// stores it by pointer, like a verify hook).
template <typename T> void register_op_interface(Context& ctx, OpId kind, const T* impl)
{
    ctx.register_interface(kind, T::kId, impl);
}
template <typename T> [[nodiscard]] const T* get_op_interface(const Context& ctx, OpId kind) noexcept
{
    return static_cast<const T*>(ctx.get_interface(kind, T::kId));
}
} // namespace crd::ceir
