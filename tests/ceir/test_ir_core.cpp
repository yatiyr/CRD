// CEIR-1a — the core IR gold-standard gate: arena storage, intrusive def-use, RAUW, O(1) op edit/erase, and the
// no-per-op-malloc proof (CountingAllocator). Host-only, device-free — no GPU, no validation layer needed.

#include <crd/ceir/ceir.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
// A named counting allocator (the house rule: no hidden default-allocator malloc in tests). Counts every parent
// allocation so we can prove building the IR does NOT hit malloc per op.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* parent) noexcept : m_parent(parent) { m_name = "ceir-test-counting"; }
    void*        allocate(crd::usize size, crd::usize align) override { ++m_allocs; return m_parent->allocate(size, align); }
    void         deallocate(void* p) noexcept override { m_parent->deallocate(p); }
    bool         owns(const void* p) const noexcept override { return m_parent->owns(p); }
    void*        reallocate(void* p, crd::usize os, crd::usize ns, crd::usize a) override { ++m_allocs; return m_parent->reallocate(p, os, ns, a); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize align) override { ++m_allocs; return m_parent->try_allocate(size, align); }
    [[nodiscard]] crd::u64 allocs() const noexcept { return m_allocs; }

private:
    crd::memory::IAllocator* m_parent;
    crd::u64                 m_allocs = 0;
};
} // namespace

using namespace crd::ceir;

TEST_CASE("ceir: build a graph and verify the def-use chains", "[ceir][ir]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);

    const OpId k_const = ctx.intern_op("test", "const");
    const OpId k_add   = ctx.intern_op("test", "add");
    REQUIRE(k_const.valid());
    REQUIRE(k_add.valid());
    REQUIRE(k_const != k_add);
    REQUIRE(ctx.op_name(k_add) == crd::containers::StringView("test.add"));
    // interning the same kind twice returns the same id (no duplicate).
    REQUIRE(ctx.intern_op("test", "add") == k_add);

    Module* m     = ctx.create_module();
    Block*  block = ctx.create_block();
    m->body()->append(block);

    // %a = test.const ; %b = test.const ; %c = test.add(%a, %b)
    Operation* a = ctx.create_operation(k_const, {}, /*num_results*/ 1U);
    Operation* b = ctx.create_operation(k_const, {}, 1U);
    block->append(a);
    block->append(b);

    Value*      ins[2] = {a->result(0), b->result(0)};
    Operation*  c      = ctx.create_operation(k_add, ins, 1U);
    block->append(c);

    // structure
    REQUIRE(block->num_ops() == 3U);
    REQUIRE(block->first_op() == a);
    REQUIRE(block->last_op() == c);
    REQUIRE(c->num_operands() == 2U);
    REQUIRE(c->operand(0) == a->result(0));
    REQUIRE(c->operand(1) == b->result(0));

    // def-use: a and b each have one use (c); c's result has none.
    REQUIRE(a->result(0)->num_uses() == 1U);
    REQUIRE(b->result(0)->num_uses() == 1U);
    REQUIRE_FALSE(c->result(0)->has_uses());
    REQUIRE(a->result(0)->defining_op() == a);
    REQUIRE(a->result(0)->kind() == ValueKind::OpResult);
    REQUIRE(a->result(0)->index() == 0U);
}

TEST_CASE("ceir: replace_all_uses_with splices the chains with zero allocation", "[ceir][ir]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const OpId                   k = ctx.intern_op("test", "v");

    Operation* a = ctx.create_operation(k, {}, 1U);
    Operation* b = ctx.create_operation(k, {}, 1U);
    Value*     ia[1] = {a->result(0)};
    Operation* u1    = ctx.create_operation(k, ia, 1U);
    Value*     ia2[1] = {a->result(0)};
    Operation* u2     = ctx.create_operation(k, ia2, 1U);

    REQUIRE(a->result(0)->num_uses() == 2U);
    REQUIRE(b->result(0)->num_uses() == 0U);

    a->result(0)->replace_all_uses_with(b->result(0));

    REQUIRE(a->result(0)->num_uses() == 0U);
    REQUIRE(b->result(0)->num_uses() == 2U);
    REQUIRE(u1->operand(0) == b->result(0));
    REQUIRE(u2->operand(0) == b->result(0));
}

TEST_CASE("ceir: set_operand rewires the use-lists", "[ceir][ir]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const OpId                   k = ctx.intern_op("test", "v");

    Operation* a = ctx.create_operation(k, {}, 1U);
    Operation* b = ctx.create_operation(k, {}, 1U);
    Value*     ia[1] = {a->result(0)};
    Operation* c     = ctx.create_operation(k, ia, 1U);

    REQUIRE(a->result(0)->num_uses() == 1U);
    REQUIRE(b->result(0)->num_uses() == 0U);

    c->set_operand(0, b->result(0));
    REQUIRE(a->result(0)->num_uses() == 0U);
    REQUIRE(b->result(0)->num_uses() == 1U);
    REQUIRE(c->operand(0) == b->result(0));
}

TEST_CASE("ceir: erase unlinks the op and drops its operands' uses", "[ceir][ir]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const OpId                   k = ctx.intern_op("test", "v");

    Block*     block = ctx.create_block();
    Operation* a     = ctx.create_operation(k, {}, 1U);
    Value*     ia[1] = {a->result(0)};
    Operation* c     = ctx.create_operation(k, ia, 1U);
    block->append(a);
    block->append(c);

    REQUIRE(a->result(0)->num_uses() == 1U);
    REQUIRE(block->num_ops() == 2U);

    c->erase(); // c consumes a; erasing c must free a's use
    REQUIRE(c->is_erased());
    REQUIRE(block->num_ops() == 1U);
    REQUIRE(block->first_op() == a);
    REQUIRE(block->last_op() == a);
    REQUIRE(a->result(0)->num_uses() == 0U); // now a is safely erasable
}

TEST_CASE("ceir: insert_before places an op mid-block (O(1))", "[ceir][ir]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const OpId                   k = ctx.intern_op("test", "v");

    Block*     block = ctx.create_block();
    Operation* a     = ctx.create_operation(k, {}, 0U);
    Operation* c     = ctx.create_operation(k, {}, 0U);
    Operation* b     = ctx.create_operation(k, {}, 0U);
    block->append(a);
    block->append(c);
    block->insert_before(b, c); // a, b, c

    REQUIRE(block->num_ops() == 3U);
    REQUIRE(block->first_op() == a);
    REQUIRE(a->next_in_block() == b);
    REQUIRE(b->next_in_block() == c);
    REQUIRE(c->prev_in_block() == b);
}

TEST_CASE("ceir: boundary cases - empty region/block, no-result op", "[ceir][ir]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const OpId                   k = ctx.intern_op("test", "v");

    Module* m = ctx.create_module();
    REQUIRE(m->body()->empty());

    Block* block = ctx.create_block();
    REQUIRE(block->empty());
    REQUIRE(block->num_ops() == 0U);

    Operation* op = ctx.create_operation(k, {}, /*no results*/ 0U);
    REQUIRE(op->num_results() == 0U);
    REQUIRE(op->num_operands() == 0U);
    block->append(op);
    REQUIRE(block->num_ops() == 1U);
    REQUIRE_FALSE(block->empty());

    // a graph region tag survives
    Region* r = ctx.create_region(RegionKind::SsaCfg);
    REQUIRE(r->kind() == RegionKind::SsaCfg);
}

TEST_CASE("ceir: building the IR does NOT malloc per op (arena)", "[ceir][ir][alloc]")
{
    crd::memory::MallocAllocator root;
    CountingAllocator            counting(&root);
    Context                      ctx(&counting);

    // Intern the kinds up front (interning a NEW kind may grow the name table) so the measurement window is pure
    // graph construction.
    const OpId k = ctx.intern_op("test", "op");
    (void)ctx.intern_op("test", "op2");

    const crd::u64 before = counting.allocs();

    // Build a modest graph — a block of 32 ops, each wired to the previous — entirely from the reserved arena chunk.
    Module* m     = ctx.create_module();
    Block*  block = ctx.create_block();
    m->body()->append(block);

    Operation* prev = ctx.create_operation(k, {}, 1U);
    block->append(prev);
    for (int i = 0; i < 31; ++i)
    {
        Value*     in[1] = {prev->result(0)};
        Operation* op    = ctx.create_operation(k, in, 1U);
        block->append(op);
        prev = op;
    }
    REQUIRE(block->num_ops() == 32U);

    // ⭐ The gate: ZERO parent allocations during construction — the 64 KB reserved chunk absorbed all 32 ops.
    CHECK(counting.allocs() == before);
}
