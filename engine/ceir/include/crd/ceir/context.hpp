#pragma once

// crd-ceir Context (CEIR-1a) — owns the IR arena + the op-kind name table, and is the ONLY factory for IR nodes.
//
// ── The arena mutation policy (the contract the whole IR is built on) ──
// Nodes are ARENA-allocated (a `crd::memory::GrowableLinearAllocator`) and NEVER freed individually. To edit the IR:
//   • change an operand     → `Operation::set_operand` (updates the def-use lists);
//   • move / insert an op   → `Block::insert_before` / `Block::append` (O(1) intrusive);
//   • remove an op          → `Operation::erase` (unlink + tombstone; its results must be use-free);
//   • grow an operand list  → rebuild the op; the old operand slice LEAKS into the arena BY DESIGN.
// Everything is reclaimed when the Context is destroyed. Node handles (`Operation*`/`Value*`/`Block*`/`Region*`)
// remain valid for the Context's entire life. Constructing the IR does NOT hit the parent allocator per op — the
// arena mallocs a chunk only ~once per thousands of nodes (a first chunk is reserved at construction).

#include <crd/ceir/id.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>

namespace crd::ceir
{
class Context
{
public:
    explicit Context(memory::IAllocator* alloc, usize arena_chunk_bytes = 64U * 1024U);
    ~Context()                         = default;
    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

    // Intern an op-kind identity: the FNV-1a hash of "dialect.op". The name is retained for diagnostics only.
    [[nodiscard]] OpId intern_op(containers::StringView dialect, containers::StringView name);
    // Reverse lookup for diagnostics — the "dialect.op" string, or "" if the id was never interned here.
    [[nodiscard]] containers::StringView op_name(OpId id) const noexcept;

    // Factories (the fluent `ModuleBuilder` is CEIR-1g). Each returns a stable arena handle.
    [[nodiscard]] Module*    create_module(RegionKind body_kind = RegionKind::Graph);
    [[nodiscard]] Region*    create_region(RegionKind kind = RegionKind::Graph);
    [[nodiscard]] Block*     create_block(u32 num_args = 0U, TypeId arg_type = {});
    [[nodiscard]] Operation* create_operation(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results,
                                              TypeId result_type = {}, u32 num_regions = 0U);

    [[nodiscard]] memory::IAllocator*             allocator() const noexcept { return m_arena.parent(); }
    [[nodiscard]] memory::GrowableLinearAllocator& arena() noexcept { return m_arena; }

private:
    struct OpName
    {
        u64                    hash = 0;
        containers::StringView name;
    };

    memory::GrowableLinearAllocator m_arena;
    containers::Array<OpName>       m_op_names;
};
} // namespace crd::ceir
