#pragma once

#include <crd/core/types.hpp>
#include <crd/scene/entity.hpp>

namespace crd::scene
{
// Relation<Tag> — the canonical relation component.
//
// A relation is `(tag, target)` where `tag` is a (typically empty) struct that
// distinguishes relation kinds and `target` is the EntityId being pointed at.
// Storing one relation on entity `src` is exactly storing a component of type
// `Relation<Tag>{target}` on `src`. The relation system is therefore not a
// new storage layer — it composes on top of L2 (ADR-0050) by treating
// relations as components-with-payload.
//
// `Relation<Tag>` is trivially relocatable (single EntityId, 8 bytes).
// Storage backends can pack it densely; the archetype path stores it inline
// in chunk SoA arrays, the sparse path stores it in dense pool arrays.
template <typename Tag>
struct Relation
{
    EntityId target = EntityId::null();

    [[nodiscard]] constexpr bool operator==(const Relation&) const noexcept = default;
};

// ---- Relation traits ----------------------------------------------------
//
// Per-relation invariants and policies, opt-in at registration time. These
// extend `ComponentInfo` (in component.hpp) with a small relation-specific
// payload. Empty trait structs are zero-cost markers; `OnTargetDestroyed`
// carries one Policy enum byte.

// Mark a relation as having a maintained reverse index (target → sources).
//
// Cost: one HashMap<EntityId, Array<EntityId>> per relation, populated by
// `add_relation` / `remove_relation`. `traverse_relation<Tag>` consumes this
// index for O(N) DFS over the subtree (parent → children walks). Without
// it, parent → children requires scanning every entity that has Relation<Tag>.
//
// Default for the canonical hierarchy / attachment / ownership relations.
struct ReverseIndex
{
};

// Mark a relation as a tree (no cycles). `add_relation` will reject any
// addition that would close a cycle. In debug builds the rejection is a
// CRD_ASSERT; the public predicate `World::would_form_cycle<Tag>(src, target)`
// allows tests and callers to check before attempting the add.
//
// In release builds the cycle check is skipped — caller is trusted.
struct Acyclic
{
};

// Policy applied when the target of a relation is destroyed. Opt-in:
// relations registered without OnTargetDestroyed leave dangling target
// references after their target dies (caller's problem).
//
// Implementation requires ReverseIndex — without it, "find every source
// pointing at the dying target" is an O(N) scan of every entity. v1f
// asserts this dependency at registration. v1g+ may relax it.
struct OnTargetDestroyed
{
    enum class Policy : crd::u8
    {
        SetNull = 0, // default: source's Relation<Tag>::target becomes EntityId::null()
        Cascade = 1, // source itself is destroyed (recursively, via the worklist)
        Detach  = 2, // Relation<Tag> component is removed from the source
    };
    Policy policy = Policy::SetNull;
};

// ---- Built-in relation tag types ---------------------------------------
//
// The canonical six relations cover every (storage × acyclic × policy)
// combination that occurs in real engine work. Each demonstrates a distinct
// trait combination; users define their own tag structs for anything else.
//
//   tag           storage    acyclic   on-destroy   typical use
//   --------      --------   -------   ----------   ---------------------
//   ChildOf       Archetype  yes       Cascade      scene tree, UI tree
//   AttachedTo    Archetype  yes       Detach       sockets, decals
//   Owns          Archetype  yes       Cascade      lifetime ownership
//   Targets       SparseSet  no        SetNull      AI lock-on, camera focus
//   DependsOn     SparseSet  yes       SetNull      asset deps, system order
//   PossessedBy   SparseSet  no        Detach       input/AI control link
//
// `World::register_builtin_relations()` registers all six with their
// canonical defaults. Users may register subsets or override individual
// defaults by calling `register_relation<Tag>(custom_traits...)` first
// (registration is idempotent — the second call is a no-op).
namespace relations
{

// Scene hierarchy. Every transform graph, UI tree, prefab, and
// replication scope hangs off ChildOf. Cascade-on-destroy: destroying a
// parent destroys its subtree.
struct ChildOf
{
};

// Socket attachment. Weapons on hands, decals on surfaces, accessories on
// joints, audio sources on emitters. Detach-on-destroy: when the socket
// entity dies, attached items survive but become unparented.
struct AttachedTo
{
};

// Lifetime ownership. Effects own particles, vehicles own wheels, scripts
// own helpers. Identical default traits to ChildOf, but semantically
// distinct: ChildOf parents transforms, Owns parents lifetimes. Splitting
// the two lets a particle parented for transform reasons not be destroyed
// when the emitter's transform parent dies (or vice versa).
struct Owns
{
};

// Tracking link. AI threat target, missile lock-on, camera focus,
// "follow this entity." SetNull-on-destroy: the source survives but its
// target becomes null and tracker code handles "lost target" gracefully.
// Cycles are allowed (A targets B targets A is a valid AI scenario).
struct Targets
{
};

// Prerequisite graph. Asset depends-on-dependency, system-runs-after-system,
// animation graph node feeding another node. Acyclic — a prerequisite
// cycle deadlocks scheduling / loading. SetNull-on-destroy.
struct DependsOn
{
};

// Control link. Player controller possesses character; AI controller
// possesses NPC; script possesses helper. Used by input routing,
// replication priority, debug overlays. Detach-on-destroy: possessor dies
// → possession ends; possessed entity survives.
struct PossessedBy
{
};

} // namespace relations
} // namespace crd::scene
