#include <crd/ceir/context.hpp>

#include <crd/containers/hash.hpp>
#include <crd/memory/construct.hpp>

namespace crd::ceir
{
Context::Context(memory::IAllocator* alloc, usize arena_chunk_bytes)
    : m_arena(arena_chunk_bytes, alloc), m_op_names(alloc) // GrowableLinearAllocator is (chunk_bytes, parent)
{
}

OpId Context::intern_op(containers::StringView dialect, containers::StringView name)
{
    // Build "dialect.op" on the stack for hashing; op names are short.
    char        buf[256];
    const usize n = dialect.size() + 1U + name.size();
    CRD_ASSERT_MSG(n < sizeof(buf), "ceir op name too long");
    usize k = 0;
    for (usize i = 0; i < dialect.size(); ++i) { buf[k++] = dialect[i]; }
    buf[k++] = '.';
    for (usize i = 0; i < name.size(); ++i) { buf[k++] = name[i]; }
    buf[k] = '\0';

    const u64 h = containers::hash_string(buf, n);
    for (usize i = 0; i < m_op_names.size(); ++i)
    {
        if (m_op_names[i].hash == h) { return OpId{h}; } // already interned — no arena/heap churn
    }
    // New kind: copy the name into the arena and record it (reverse lookup for diagnostics).
    char* const stored = static_cast<char*>(m_arena.allocate(n + 1U, 1U));
    for (usize i = 0; i <= n; ++i) { stored[i] = buf[i]; }
    m_op_names.push_back(OpName{h, containers::StringView(stored, n)});
    return OpId{h};
}

containers::StringView Context::op_name(OpId id) const noexcept
{
    for (usize i = 0; i < m_op_names.size(); ++i)
    {
        if (m_op_names[i].hash == id.value) { return m_op_names[i].name; }
    }
    return containers::StringView{};
}

Module* Context::create_module(RegionKind body_kind)
{
    Module* const m = memory::construct<Module>(m_arena);
    m->m_body       = create_region(body_kind);
    return m;
}

Region* Context::create_region(RegionKind kind)
{
    Region* const r = memory::construct<Region>(m_arena);
    r->m_kind       = kind;
    return r;
}

Block* Context::create_block(u32 num_args, TypeId arg_type)
{
    Block* const b = memory::construct<Block>(m_arena);
    b->m_num_args  = num_args;
    b->m_args      = memory::construct_array<Value>(m_arena, num_args);
    for (u32 i = 0; i < num_args; ++i) { b->m_args[i].init(arg_type, ValueKind::BlockArg, i, b); }
    return b;
}

Operation* Context::create_operation(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results,
                                     TypeId result_type, u32 num_regions)
{
    Operation* const op = memory::construct<Operation>(m_arena);
    op->m_kind          = kind;

    op->m_num_results = num_results;
    op->m_results     = memory::construct_array<Value>(m_arena, num_results);
    for (u32 i = 0; i < num_results; ++i) { op->m_results[i].init(result_type, ValueKind::OpResult, i, op); }

    op->m_num_regions = num_regions;
    op->m_regions     = memory::construct_array<Region*>(m_arena, num_regions); // trivially-init; overwritten below
    for (u32 i = 0; i < num_regions; ++i) { op->m_regions[i] = create_region(); }

    const auto num_operands = static_cast<u32>(operands.size());
    op->m_num_operands      = num_operands;
    op->m_operands          = memory::construct_array<Use>(m_arena, num_operands);
    for (u32 i = 0; i < num_operands; ++i) { op->set_operand(i, operands[i]); } // wires the def-use lists

    return op;
}
} // namespace crd::ceir
