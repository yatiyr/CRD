// Phase 3.0 v1h — Commands implementation (ADR-0052 §5).
//
// Non-template body for queue/flush/destruct. Templated mutations live in
// world.hpp's inline section because they need World::require_component_id
// and the storage backend dispatchers.

#include <crd/core/assert.hpp>
#include <crd/scene/commands.hpp>
#include <crd/scene/world.hpp>

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

void Commands::enqueue_with_payload(Command cmd, void* src_value, const ComponentInfo* info)
{
    CRD_ASSERT(info != nullptr);
    CRD_ASSERT(src_value != nullptr);

    // Reserve `info->size` bytes in m_payloads at the right alignment.
    // m_payloads grows by single bytes; we round up to alignment manually.
    crd::usize offset = m_payloads.size();
    const crd::usize misalign = offset % info->alignment;
    if (misalign != 0)
    {
        offset += info->alignment - misalign;
    }
    m_payloads.resize(offset + info->size);

    // Move-construct the value into the reserved bytes. Fall back to
    // memcpy for trivially-movable types where move_construct wasn't
    // captured at registration.
    crd::u8* dst = &m_payloads[offset];
    if (info->move_construct != nullptr)
    {
        info->move_construct(dst, src_value);
    }
    else
    {
        std::memcpy(dst, src_value, info->size);
    }

    cmd.payload_offset = static_cast<crd::u32>(offset);
    cmd.payload_size   = static_cast<crd::u32>(info->size);
    m_commands.push_back(cmd);
}

void Commands::destruct_unflushed_payloads() noexcept
{
    for (Command& cmd : m_commands)
    {
        if (cmd.payload_offset == 0xFFFFFFFFU)
        {
            continue;
        }
        const ComponentInfo* info = m_world->components().info(cmd.component);
        if (info == nullptr || info->destruct == nullptr)
        {
            continue;
        }
        info->destruct(&m_payloads[cmd.payload_offset]);
        cmd.payload_offset = 0xFFFFFFFFU;
    }
    m_commands.clear();
    m_payloads.clear();
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
                    // Entity was destroyed earlier in the same flush —
                    // run the payload destructor manually so we don't leak.
                    const ComponentInfo* info = m_world->components().info(cmd.component);
                    if (info != nullptr && info->destruct != nullptr)
                    {
                        info->destruct(&m_payloads[cmd.payload_offset]);
                    }
                    cmd.payload_offset = 0xFFFFFFFFU;
                    break;
                }
                // Storage UPSERTs through insert(); the backend's
                // move_construct callback consumes the payload bytes.
                m_world->backend_for_public(cmd.component)
                    .insert(cmd.entity, cmd.component, &m_payloads[cmd.payload_offset]);
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

    // Some payloads may still hold T values if the entity was dead at
    // flush time (handled inline above). The rest were move-consumed by
    // the storage backends. Clear the buffers safely.
    destruct_unflushed_payloads();
}

} // namespace crd::scene
