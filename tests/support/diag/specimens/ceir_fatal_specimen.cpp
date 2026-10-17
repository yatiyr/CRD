// CEIR context contract fatals (crd-ceir-fatal-* CTests, tests/execution/ceir). The context's IR arena grows from its
// parent allocator and refuses only when the parent does: the context is then out of memory, fatal in every build
// (DIAG.3a's policy for an owning arena), never a null operation, block, region or string written through (the
// builders wrote through null in a build without asserts before). One mode per run (argv[1]); see expected_fatal.hpp
// for the exit protocol:
//   create-operation-arena-refused  create_operation once the parent refuses the arena's next chunk
//   create-block-arena-refused      create_block once the parent refuses the arena's next chunk
//   intern-symbol-arena-refused     intern_symbol of a name larger than any chunk once the parent refuses
// func::func_body is the entry-block accessor for a func.func that has one (every create_func op does): an op without
// a body is fatal, never a null block followed:
//   func-body-missing               func_body on an op with no regions
// The authored-rule driver walks operand(operand_idx).def.operand(inner) only under the constraint that proves the
// path; a rule that reaches rule_apply without it is a driver-contract violation, fatal, never a null op followed:
//   rewrite-inner-op-missing        an inner-operand rule applied to an op whose operand is a block argument

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "specimen_common.hpp"

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/passes/rewrite_rules.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace
{
namespace ce = crd::ceir;

// A parent that grants from the default allocator until refuse() and returns nullptr from then on, as an exhausted
// arena does.
class SwitchableParent final : public crd::memory::IAllocator
{
public:
    SwitchableParent() noexcept { m_name = "ceir-fatal-parent"; }

    void* allocate(crd::usize size, crd::usize alignment) override { return try_allocate(size, alignment); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        return m_refusing ? nullptr : crd::memory::default_allocator()->try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { crd::memory::default_allocator()->deallocate(p); }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return p != nullptr; }

    void refuse() noexcept { m_refusing = true; }

private:
    bool m_refusing = false;
};

constexpr crd::usize kChunkBytes = 1024U; // small, so a few nodes need the next chunk
constexpr int kMaxNodes = 4096;           // far more than one chunk holds: the fatal must come long before

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    SwitchableParent parent;
    if (is(mode, "create-operation-arena-refused"))
    {
        crd_diag_fatal::arm(mode, ce::detail::kContextArenaOutOfMemory);
        ce::Context ctx(&parent, kChunkBytes);
        const ce::OpId kind = ctx.intern_op("specimen", "node");
        const ce::TypeId i64 = ctx.type_i64();
        parent.refuse();
        for (int i = 0; i < kMaxNodes; ++i)
        {
            (void)ctx.create_operation(kind, crd::containers::ConstSpan<ce::Value*>{}, 1U, i64, 0U);
        }
        return true;
    }
    if (is(mode, "create-block-arena-refused"))
    {
        crd_diag_fatal::arm(mode, ce::detail::kContextArenaOutOfMemory);
        ce::Context ctx(&parent, kChunkBytes);
        const ce::TypeId i64 = ctx.type_i64();
        parent.refuse();
        for (int i = 0; i < kMaxNodes; ++i)
        {
            (void)ctx.create_block(2U, i64);
        }
        return true;
    }
    if (is(mode, "intern-symbol-arena-refused"))
    {
        crd_diag_fatal::arm(mode, ce::detail::kContextArenaOutOfMemory);
        ce::Context ctx(&parent, kChunkBytes);
        static char name[2U * kChunkBytes]; // larger than a chunk: it needs a chunk of its own
        std::memset(name, 'n', sizeof(name));
        parent.refuse();
        return !ctx.intern_symbol(crd::containers::StringView{name, sizeof(name)}).empty();
    }
    if (is(mode, "func-body-missing"))
    {
        crd_diag_fatal::arm(mode, "func::func_body: the op has no body block");
        ce::Context ctx(&parent, kChunkBytes);
        ce::Operation* const op =
            ctx.create_operation(ctx.intern_op("specimen", "bodiless"), crd::containers::ConstSpan<ce::Value*>{}, 0U);
        return ce::func::func_body(*op).num_args() != 0U;
    }
    if (is(mode, "rewrite-inner-op-missing"))
    {
        crd_diag_fatal::arm(mode, "greedy_rewrite_rules: an inner-operand rule met an operand without a defining op");
        ce::Context ctx(&parent, kChunkBytes);
        const ce::TypeId i64 = ctx.type_i64();
        ce::Block* const block = ctx.create_block(1U, i64);
        const ce::OpId wrap = ctx.intern_op("specimen", "wrap");
        ce::Value* in[1] = {block->arg(0U)};
        ce::Operation* const op = ctx.create_operation(wrap, crd::containers::ConstSpan<ce::Value*>{in, 1U}, 1U, i64);
        ce::Value* use[1] = {op->result(0U)};
        (void)ctx.create_operation(ctx.intern_op("specimen", "sink"), crd::containers::ConstSpan<ce::Value*>{use, 1U},
                                   0U);
        // The type constraint matches (result and operand are both i64), but it proves nothing about the operand's
        // definer, which find_rewrite_misuse would have rejected for this action.
        ce::rewrite::RewriteRule rule{};
        rule.root_kind = wrap;
        rule.constraint = ce::rewrite::RewriteConstraint::ResultTypeEqOperand;
        rule.action = ce::rewrite::RewriteAction::BuildOpFromInnerOperand;
        if (!ce::rr_detail::rule_matches(ctx, rule, *op))
        {
            return true; // no match, no fatal: the CTest rejects the clean exit
        }
        ce::rr_detail::rule_apply(ctx, rule, *op);
        return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
