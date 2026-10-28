// Phase 3.0 v1h — Commands implementation (ADR-0052 §5).
//
// Non-template body for queue/flush/destruct. Templated mutations live in
// world.hpp's inline section because they need World::require_component_id
// and the storage backend dispatchers.

#include <crd/core/assert.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/scene/commands.hpp>
#include <crd/scene/world.hpp>

#include <cstdint>

namespace crd::scene
{

Commands::Commands(World& world)
    : m_world(&world), m_commands(world.allocator()), m_payloads(world.allocator())
{
}

Commands::~Commands()
{
    // Drain destructors on remaining payloads so non-trivially-destructible
    // T values don't leak. Normal frames flush before this runs.
    destruct_unflushed_payloads();
}

EntityId Commands::spawn()
{
    // Immediate (single-threaded v1h). v1h+1 will swap to a deferred-with-
    // placeholder path when par_each can run from multiple fibers.
    return m_world->spawn();
}

void Commands::destroy(EntityId e)
{
    Command cmd{};
    cmd.kind   = CommandKind::Destroy;
    cmd.entity = e;
    enqueue(cmd);
}

void Commands::enqueue(Command cmd)
{
    m_commands.push_back(cmd);
}

void Commands::enqueue_with_payload(Command cmd, void* src_value, const ComponentInfo& info)
{
    CRD_ASSERT(src_value != nullptr);
    CRD_ASSERT(info.alignment != 0U && info.alignment <= kMaxComponentAlignment);

    // An offset aligned to the type is an aligned address: the buffer starts on a kMaxComponentAlignment block.
    const crd::usize offset = crd::memory::align_up(m_payload_bytes, info.alignment);
    const crd::usize end = offset + info.size;
    if (end >= 0xFFFFFFFFU)
    {
        CRD_FATAL("Commands: queued component payloads exceed the 4 GiB offset range");
    }
    const crd::usize blocks = (end + kMaxComponentAlignment - 1U) / kMaxComponentAlignment;
    if (blocks > m_payloads.capacity())
    {
        grow_payloads(blocks);
    }
    if (blocks > m_payloads.size())
    {
        m_payloads.resize_uninitialized(blocks);
    }
    m_payload_bytes = end;

    // Move-construct the value into the reserved bytes. Fall back to
    // memcpy for trivially-movable types where move_construct wasn't
    // captured at registration.
    crd::u8* dst = payload_at(static_cast<crd::u32>(offset));
    CRD_ASSERT(reinterpret_cast<std::uintptr_t>(dst) % info.alignment == 0U);
    if (info.move_construct != nullptr)
    {
        info.move_construct(dst, src_value);
    }
    else
    {
        std::memcpy(dst, src_value, info.size);
    }

    cmd.payload_offset = static_cast<crd::u32>(offset);
    cmd.payload_size = static_cast<crd::u32>(info.size);
    m_commands.push_back(cmd);
}

void Commands::grow_payloads(crd::usize min_blocks)
{
    crd::usize capacity = m_payloads.capacity() * 2U;
    if (capacity < min_blocks)
    {
        capacity = min_blocks;
    }
    crd::containers::Array<PayloadBlock> grown(m_world->allocator());
    grown.reserve(capacity);
    grown.resize_uninitialized(m_payloads.size());
    // Every live payload moves to the same offset, through its type: a byte copy would break a type that points into
    // itself. A type without a move constructor was placed by a byte copy, so it moves the same way.
    crd::u8* const to = reinterpret_cast<crd::u8*>(grown.data());
    for (const Command& cmd : m_commands)
    {
        if (cmd.payload_offset == 0xFFFFFFFFU)
        {
            continue;
        }
        const ComponentInfo& info = m_world->components().require_info(cmd.component);
        crd::u8* const from = payload_at(cmd.payload_offset);
        if (info.move_construct != nullptr)
        {
            info.move_construct(to + cmd.payload_offset, from);
            if (info.destruct != nullptr)
            {
                info.destruct(from);
            }
        }
        else
        {
            std::memcpy(to + cmd.payload_offset, from, cmd.payload_size);
        }
    }
    m_payloads = std::move(grown);
}

void Commands::destroy_payload(Command& cmd) noexcept
{
    const ComponentInfo* info = m_world->components().info(cmd.component);
    if (info != nullptr && info->destruct != nullptr)
    {
        info->destruct(payload_at(cmd.payload_offset));
    }
    cmd.payload_offset = 0xFFFFFFFFU;
}

void Commands::destruct_unflushed_payloads() noexcept
{
    for (Command& cmd : m_commands)
    {
        if (cmd.payload_offset != 0xFFFFFFFFU)
        {
            destroy_payload(cmd);
        }
    }
    m_commands.clear();
    m_payloads.clear();
    m_payload_bytes = 0;
}

void Commands::flush()
{
    if (m_commands.size() == 0)
    {
        return;
    }
    for (Command& cmd : m_commands)
    {
        switch (cmd.kind)
        {
            case CommandKind::Destroy:
                m_world->destroy_immediate(cmd.entity);
                break;

            case CommandKind::AddComponent:
            {
                CRD_ASSERT(cmd.payload_offset != 0xFFFFFFFFU);
                if (!m_world->is_alive(cmd.entity))
                {
                    // Entity was destroyed earlier in the same flush: the payload is never stored.
                    destroy_payload(cmd);
                    break;
                }
                // Storage UPSERTs through insert(), which moves from the payload; the moved-from payload is then
                // destroyed. A type without a move constructor is stored by a byte copy, which takes its ownership.
                const ComponentInfo& info = m_world->components().require_info(cmd.component);
                m_world->backend_for_public(cmd.component)
                    .insert(cmd.entity, cmd.component, payload_at(cmd.payload_offset));
                if (info.move_construct != nullptr)
                {
                    destroy_payload(cmd);
                }
                cmd.payload_offset = 0xFFFFFFFFU;
                break;
            }

            case CommandKind::RemoveComponent:
                if (m_world->is_alive(cmd.entity))
                {
                    m_world->backend_for_public(cmd.component).remove(cmd.entity, cmd.component);
                }
                break;

            case CommandKind::AddRelation:
                if (m_world->is_alive(cmd.entity))
                {
                    // Reuse World's add_relation_impl. ComponentId carries
                    // the Relation<Tag> identity (resolved at queue time).
                    m_world->add_relation_via_id(cmd.component, cmd.entity, cmd.relation_target);
                }
                break;

            case CommandKind::RemoveRelation:
                if (m_world->is_alive(cmd.entity))
                {
                    m_world->remove_relation_via_id(cmd.component, cmd.entity);
                }
                break;
        }
    }

    // Every payload was stored or destroyed above; this clears the buffers.
    destruct_unflushed_payloads();
}

} // namespace crd::scene
