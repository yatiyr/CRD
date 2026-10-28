#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/relation.hpp>

#include <cstring>
#include <type_traits>
#include <utility>

namespace crd::scene
{
class World;

// Commands — Phase 3.0 v1h (ADR-0052 §5).
//
// Deferred-mutation buffer used by systems that iterate the world. The
// schedule drains commands at every phase boundary, applying them in
// registration order on the calling thread.
//
// Operations:
//   spawn()                                 — IMMEDIATE: allocates from
//                                             World::SlotMap right now;
//                                             returns a real, usable EntityId.
//   destroy(e)                              — DEFERRED: applied on flush().
//   add_component<T>(e, value)              — DEFERRED.
//   remove_component<T>(e)                  — DEFERRED.
//   set_component<T>(e, value)              — DEFERRED (UPSERT).
//   add_relation<Tag>(src, target)          — DEFERRED.
//   remove_relation<Tag>(src)               — DEFERRED.
//
// Why spawn is immediate while mutations are deferred:
//   v1h ships SERIAL phase dispatch — only one system runs at a time, so
//   immediate slot-allocation is race-free. Mutations are deferred so
//   that within a phase, an iteration body that queues changes doesn't
//   reshape the iteration's storage mid-walk. The pattern matches Bevy's
//   `Commands::spawn` shape (lazy mutation, real id).
//
//   When v1h+1 enables parallel par_each, spawn will need to switch to a
//   deferred path with a placeholder handle resolved at flush. v1h's
//   API stays the same (return type stays EntityId); the
//   implementation gets a per-fiber stripe.
//
// Ownership / lifetime:
//   Commands holds an `Array<Command>` of records and a parallel payload
//   buffer of kMaxComponentAlignment-aligned blocks, each payload
//   move-constructed from the user's value at queue time at an offset
//   aligned to its type. A buffer growth relocates every live payload
//   through its type's move constructor and destructor, never by a byte
//   copy. At flush, each payload is the source of the storage backend's
//   insert() and is destroyed afterwards. If Commands is destroyed without
//   flush, the destructor runs every remaining payload's destructor.
class Commands
{
public:
    explicit Commands(World& world);
    ~Commands();

    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
    Commands(Commands&&) = delete;
    Commands& operator=(Commands&&) = delete;

    // ---- Immediate ops -------------------------------------------------

    [[nodiscard]] EntityId spawn();

    // ---- Deferred ops -------------------------------------------------

    void destroy(EntityId e);

    // An lvalue is copied once; an rvalue is the payload's move source (World::add_component).
    template <typename T> void add_component(EntityId e, const T& value);
    template <typename T>
        requires(std::is_same_v<T, std::remove_cvref_t<T>>)
    void add_component(EntityId e, T&& value);
    template <typename T> void remove_component(EntityId e);
    template <typename T> void set_component(EntityId e, const T& value);
    template <typename T>
        requires(std::is_same_v<T, std::remove_cvref_t<T>>)
    void set_component(EntityId e, T&& value);

    template <typename Tag> void add_relation(EntityId src, EntityId target);
    template <typename Tag> void remove_relation(EntityId src);

    // ---- Drain --------------------------------------------------------

    void flush();
    [[nodiscard]] bool empty() const noexcept { return m_commands.size() == 0; }
    [[nodiscard]] crd::usize pending() const noexcept { return m_commands.size(); }

private:
    enum class CommandKind : crd::u8
    {
        Destroy         = 0,
        AddComponent    = 1, // == SetComponent (storage UPSERT semantics)
        RemoveComponent = 2,
        AddRelation     = 3,
        RemoveRelation  = 4,
    };

    struct Command
    {
        CommandKind kind;
        ComponentId component{};       // for component / relation ops
        EntityId    entity{};
        EntityId    relation_target{}; // for AddRelation
        crd::u32    payload_offset = 0xFFFFFFFFU;
        crd::u32    payload_size   = 0;
    };

    // Push a Command record without payload (destroy / remove_component /
    // remove_relation / add_relation).
    void enqueue(Command cmd);

    // Push an AddComponent record with a move-constructed payload of size
    // `info.size`. ComponentInfo is looked up from the registry at queue
    // time and re-resolved at flush; the ID is the bridge.
    void enqueue_with_payload(Command cmd, void* src_value, const ComponentInfo& info);

    // Reallocate the payload buffer to hold at least `min_blocks`, relocating every live payload.
    void grow_payloads(crd::usize min_blocks);

    // Run the payload's destructor and mark the command as holding none.
    void destroy_payload(Command& cmd) noexcept;

    [[nodiscard]] crd::u8* payload_at(crd::u32 offset) noexcept
    {
        return reinterpret_cast<crd::u8*>(m_payloads.data()) + offset;
    }

    // Run any registered destructors on remaining command payloads. Called
    // from ~Commands and at the end of flush() so the buffer can be
    // re-used safely.
    void destruct_unflushed_payloads() noexcept;

    // A payload at an offset aligned to its type is aligned in memory: the buffer starts on a block.
    struct alignas(kMaxComponentAlignment) PayloadBlock
    {
        crd::u8 bytes[kMaxComponentAlignment];
    };

    World* m_world;
    crd::containers::Array<Command> m_commands;
    crd::containers::Array<PayloadBlock> m_payloads;
    crd::usize m_payload_bytes = 0; // bytes in use, a prefix of m_payloads
};

// ---- Inline templated mutations -----------------------------------------
//
// Templates depend on World template methods (require_component_id,
// component_id), so their bodies live in commands_inl.hpp at the bottom
// of world.hpp's include chain (after World is complete). For now, just
// declare them in this header.

} // namespace crd::scene
