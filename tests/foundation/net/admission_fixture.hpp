#pragma once

// What every admission test holds: a signed schema's artefact and a capability for a hand-written schema, and an
// allocator that counts its buffers.

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/net/admission.hpp>

#include <cstring>

namespace crd::net::test
{

inline crypto::AuthorizedSchema authorized_for(const wire::WireSchema& wire)
{
    crypto::AuthorizedSchema schema;
    schema.schema.kind = static_cast<crypto::ScopeKind>(wire.kind);
    std::memcpy(schema.schema.schema.bytes, wire.id, sizeof(wire.id));
    schema.schema.major = wire.major;
    schema.schema.minor = wire.minor;
    schema.schema.audience.bytes[0] = 7U;
    schema.schema.operations = wire.operations;
    schema.schema.idempotent = wire.idempotent;
    return schema;
}

inline crypto::Capability capability_for(const crypto::AuthorizedSchema& schema, u64 operations = ~u64{0})
{
    crypto::Capability capability;
    capability.audience = schema.schema.audience;
    capability.grant.kind = schema.schema.kind;
    capability.grant.operations = operations;
    capability.grant.budget.operations_per_second = 1000000U;
    capability.grant.budget.bytes_per_second = 1000000000ULL;
    capability.grant.budget.objects = 100U;
    return capability;
}

// A counting allocator: every buffer a test does not expect shows, and any request can be refused.
class Buffers final : public memory::IAllocator
{
public:
    Buffers() : m_heap(4U * 1024U * 1024U) {}

    void* allocate(usize size, usize alignment) override { return try_allocate(size, alignment); }

    void deallocate(void* p) noexcept override
    {
        if (p != nullptr)
        {
            --live;
            m_heap.deallocate(p);
        }
    }

    bool owns(const void* p) const noexcept override { return m_heap.owns(p); }

    void* try_allocate(usize size, usize alignment) override
    {
        if (refuse_all)
        {
            return nullptr;
        }
        ++total;
        ++live;
        return m_heap.try_allocate(size, alignment);
    }

    bool refuse_all = false;
    u32 total = 0U;
    u32 live = 0U;

private:
    memory::TlsfAllocator m_heap;
};

} // namespace crd::net::test
