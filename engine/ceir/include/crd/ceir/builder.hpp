#pragma once

// crd-ceir — the ModuleBuilder fluent C++ API (CEIR-1g, §121). An ergonomic, chainable host-side builder for
// constructing modules. ⛔⛔ NO PRIVILEGED BYPASS: every op is created through the SAME `Context::create_operation`
// the hand path uses, placed with the SAME intrusive block edits, and (for a symbol-defining op) registered through
// the SAME `detail::register_symbol` — so a builder-made module is INDISTINGUISHABLE from a hand-built one and, like
// the hand path, is not auto-verified. `verify()` is an explicit convenience that dispatches the REAL per-kind
// `VerifyFn` via `Context::verify` (proven by a rejection test), never a stub.
//
//   ModuleBuilder mb(ctx);
//   Block* top = mb.add_block(1U, TypeId{1U});                       // ^bb0(%0 : !t1)
//   Value* s   = mb.op("test","add").operand(top->arg(0)).operand(top->arg(0))
//                  .result(TypeId{2U}).attr("k", ctx.attr_int(7)).build_result();
//   Operation* fn = mb.func("f", Visibility::Public, 1U, TypeId{1U});
//   { InsertionGuard g(mb); mb.set_insertion(func::func_body_block(fn)); mb.ret({fn body arg}); }

#include <crd/ceir/attr.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir
{
class ModuleBuilder;

// A fluent op-emission proxy. Accumulates operands / results / attributes / regions, then a terminal `build()` creates
// the op through `Context::create_operation`, appends it at the builder's insertion point, applies the attributes +
// source loc, and re-registers a symbol-defining op. Chain methods return `*this`; obtain one from `ModuleBuilder::op`.
class OpBuilder
{
public:
    OpBuilder(ModuleBuilder& mb, OpId kind);

    OpBuilder& operand(Value* v);
    OpBuilder& operands(containers::ConstSpan<Value*> vs);
    OpBuilder& result(TypeId t = {});                    // one result of type `t`
    OpBuilder& results(u32 n, TypeId t = {});            // `n` results, all of type `t` (create_operation's uniform shape)
    OpBuilder& attr(containers::StringView name, AttrId value); // applied in CALL order (the dict's insertion order)
    OpBuilder& regions(u32 n);
    OpBuilder& loc(SourceLoc l);

    // Create the op and place it at the insertion point. Returns the op, or NULLPTR if it defines a `sym_name` that is
    // already taken (the op is erased — no silent overwrite, mirroring `func::create_func`).
    Operation* build();
    // Build, then return result `i`. Asserts the build succeeded (do not use on a symbol-defining op that may clash).
    [[nodiscard]] Value* build_result(u32 i = 0U);

private:
    ModuleBuilder&               m_mb;
    OpId                         m_kind;
    containers::Array<Value*>    m_operands;
    containers::Array<NamedAttr> m_attrs;
    u32                          m_num_results = 0U;
    TypeId                       m_result_type{};
    u32                          m_num_regions = 0U;
    SourceLoc                    m_loc{};
};

class ModuleBuilder
{
public:
    // Create a fresh module (body region `body_kind`) owned by `ctx`. There is no insertion point until `add_block`.
    explicit ModuleBuilder(Context& ctx, RegionKind body_kind = RegionKind::Graph);

    [[nodiscard]] Context& context() const noexcept { return m_ctx; }
    [[nodiscard]] Module*  module() const noexcept { return m_module; }

    // Create a block (typed args) in `into` (default: the module body), append it, and make it the insertion point.
    Block*                add_block(u32 num_args = 0U, TypeId arg_type = {}, Region* into = nullptr);
    void                  set_insertion(Block* b) noexcept { m_insert = b; }
    [[nodiscard]] Block*  insertion() const noexcept { return m_insert; }

    // Begin a fluent op at the insertion point.
    [[nodiscard]] OpBuilder op(containers::StringView dialect, containers::StringView name);

    // `ceir.func` convenience (reuses crd::ceir::func) — each is placed at the insertion point. `func` returns nullptr
    // on a duplicate/empty name (create_func's contract); fill its body by pointing the insertion at its entry block.
    Operation* func(containers::StringView name, Visibility vis, u32 num_params, TypeId param_type = {});
    Operation* ret(containers::ConstSpan<Value*> values);
    Operation* call(containers::StringView callee, containers::ConstSpan<Value*> args, u32 num_results,
                    TypeId result_type = {});

    // Verify EVERY op (recursively) against its kind's registered verifier — the real `Context::verify`, no bypass.
    // On failure returns false and, if `failing` is non-null, sets it to the first offending op.
    [[nodiscard]] bool verify(const Operation** failing = nullptr) const;

private:
    Context& m_ctx;
    Module*  m_module;
    Block*   m_insert = nullptr;
};

// RAII save/restore of a builder's insertion point — build into a nested region, then return to where you were.
class InsertionGuard
{
public:
    explicit InsertionGuard(ModuleBuilder& mb) noexcept : m_mb(mb), m_saved(mb.insertion()) {}
    ~InsertionGuard() { m_mb.set_insertion(m_saved); }
    InsertionGuard(const InsertionGuard&)            = delete;
    InsertionGuard& operator=(const InsertionGuard&) = delete;
    InsertionGuard(InsertionGuard&&)                 = delete;
    InsertionGuard& operator=(InsertionGuard&&)      = delete;

private:
    ModuleBuilder& m_mb;
    Block*         m_saved;
};
} // namespace crd::ceir
