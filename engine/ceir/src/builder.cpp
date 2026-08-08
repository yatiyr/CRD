#include <crd/ceir/builder.hpp>

#include <crd/ceir/detail/symbol_registration.hpp>
#include <crd/core/assert.hpp>

namespace crd::ceir
{
// ─────────────────────────────────── ModuleBuilder ───────────────────────────────────
ModuleBuilder::ModuleBuilder(Context& ctx, RegionKind body_kind)
    : m_ctx(ctx), m_module(ctx.create_module(body_kind))
{
}

Block* ModuleBuilder::add_block(u32 num_args, TypeId arg_type, Region* into)
{
    Region* const r = into != nullptr ? into : m_module->body();
    Block* const  b = m_ctx.create_block(num_args, arg_type);
    r->append(b);
    m_insert = b;
    return b;
}

OpBuilder ModuleBuilder::op(containers::StringView dialect, containers::StringView name)
{
    return OpBuilder(*this, m_ctx.intern_op(dialect, name));
}

Operation* ModuleBuilder::func(containers::StringView name, Visibility vis, u32 num_params, TypeId param_type)
{
    // Assert BEFORE create_func — it registers the symbol as a side effect, so bailing after would leave a
    // registered-but-unplaced func in the table.
    CRD_ASSERT_MSG(m_insert != nullptr, "ModuleBuilder::func needs an insertion block (call add_block first)");
    Operation* const fn = func::create_func(m_ctx, *m_module, name, vis, num_params, param_type);
    if (fn != nullptr) { m_insert->append(fn); }
    return fn;
}

Operation* ModuleBuilder::ret(containers::ConstSpan<Value*> values)
{
    CRD_ASSERT_MSG(m_insert != nullptr, "ModuleBuilder::ret needs an insertion block");
    Operation* const op = func::create_return(m_ctx, values);
    m_insert->append(op);
    return op;
}

Operation* ModuleBuilder::call(containers::StringView callee, containers::ConstSpan<Value*> args, u32 num_results,
                               TypeId result_type)
{
    CRD_ASSERT_MSG(m_insert != nullptr, "ModuleBuilder::call needs an insertion block");
    Operation* const op = func::create_call(m_ctx, callee, args, num_results, result_type);
    m_insert->append(op);
    return op;
}

namespace
{
[[nodiscard]] bool verify_region(const Context& ctx, Region* r, const Operation** failing)
{
    if (r == nullptr) { return true; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (!ctx.verify(*op))
            {
                if (failing != nullptr) { *failing = op; }
                return false;
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                if (!verify_region(ctx, op->region(i), failing)) { return false; }
            }
        }
    }
    return true;
}
} // namespace

bool ModuleBuilder::verify(const Operation** failing) const
{
    return verify_region(m_ctx, m_module->body(), failing);
}

// ───────────────────────────────────── OpBuilder ─────────────────────────────────────
OpBuilder::OpBuilder(ModuleBuilder& mb, OpId kind)
    : m_mb(mb), m_kind(kind), m_operands(mb.context().allocator()), m_attrs(mb.context().allocator())
{
}

OpBuilder& OpBuilder::operand(Value* v)
{
    m_operands.push_back(v);
    return *this;
}
OpBuilder& OpBuilder::operands(containers::ConstSpan<Value*> vs)
{
    for (usize i = 0; i < vs.size(); ++i) { m_operands.push_back(vs[i]); }
    return *this;
}
OpBuilder& OpBuilder::result(TypeId t)
{
    m_num_results = 1U;
    m_result_type = t;
    return *this;
}
OpBuilder& OpBuilder::results(u32 n, TypeId t)
{
    m_num_results = n;
    m_result_type = t;
    return *this;
}
OpBuilder& OpBuilder::attr(containers::StringView name, AttrId value)
{
    m_attrs.push_back(NamedAttr{name, value});
    return *this;
}
OpBuilder& OpBuilder::regions(u32 n)
{
    m_num_regions = n;
    return *this;
}
OpBuilder& OpBuilder::loc(SourceLoc l)
{
    m_loc = l;
    return *this;
}

Operation* OpBuilder::build()
{
    Context&     ctx    = m_mb.context();
    Block* const insert = m_mb.insertion();
    CRD_ASSERT_MSG(insert != nullptr, "OpBuilder::build needs an insertion block (call add_block first)");
    // The SAME factory the hand path uses — no privileged construction.
    Operation* const op = ctx.create_operation(m_kind, containers::ConstSpan<Value*>(m_operands.data(), m_operands.size()),
                                               m_num_results, m_result_type, m_num_regions);
    insert->append(op);
    for (usize i = 0; i < m_attrs.size(); ++i) { ctx.set_attr(op, m_attrs[i].name, m_attrs[i].value); }
    op->set_loc(m_loc);
    if (!detail::register_symbol(ctx, *m_mb.module(), op))
    {
        op->erase(); // duplicate sym_name — undo (results are use-free here; the op tombstones into the arena)
        return nullptr;
    }
    return op;
}

Value* OpBuilder::build_result(u32 i)
{
    Operation* const op = build();
    CRD_ASSERT_MSG(op != nullptr, "OpBuilder::build_result on an op that failed to build (duplicate symbol name?)");
    return op->result(i);
}
} // namespace crd::ceir
