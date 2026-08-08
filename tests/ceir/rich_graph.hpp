#pragma once

// A dense CEIR module fixture shared by the CEIR-1e text round-trip gate (test_roundtrip.cpp) and the CEIR-1f binary
// round-trip gate (test_binary.cpp). It touches every construct both serial forms must preserve: results / operands,
// attributes of every kind (incl. a negative int, "4.0"/exponent floats, an escaped string with quote+backslash+brace,
// a symbol ref, a type), nested + empty + multi-block regions, a use-before-def operand (the Graph-region fixup path),
// an unregistered-dialect opaque op, and func.func/call/return. Host-only.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>

namespace crd::ceir::test
{
// Build the dense fixture into `ctx`. Returns the module.
inline Module* build_rich(Context& ctx)
{
    using crd::containers::ConstSpan;

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(1U, TypeId{1U}); // ^bb0(%0 : !t1)
    m->body()->append(top);
    Value* const a0 = top->arg(0U);

    // %r = test.add(%0, %0) : !t2  with an attribute of EVERY kind (set out of name order on purpose).
    Value*           add_ops[2] = {a0, a0};
    Operation* const add =
        ctx.create_operation(ctx.intern_op("test", "add"), ConstSpan<Value*>(add_ops, 2U), 1U, TypeId{2U});
    ctx.set_attr(add, "i", ctx.attr_int(-42));                     // negative int
    ctx.set_attr(add, "frac", ctx.attr_float(4.5));
    ctx.set_attr(add, "whole", ctx.attr_float(4.0));               // must survive as "4.0", re-read as float
    ctx.set_attr(add, "big", ctx.attr_float(1.0e20));             // exponent float
    ctx.set_attr(add, "flag", ctx.attr_bool(true));
    ctx.set_attr(add, "s", ctx.attr_string(R"(he said "hi"\n)")); // embedded quote + backslash (raw literal)
    ctx.set_attr(add, "sym", ctx.attr_symbol("target"));
    ctx.set_attr(add, "ty", ctx.attr_type(TypeId{7U}));
    top->append(add);
    Value* const r = add->result(0U);

    // multi-result, NO result type: %a, %b = test.split(%r)
    Value*           split_ops[1] = {r};
    Operation* const split = ctx.create_operation(ctx.intern_op("test", "split"), ConstSpan<Value*>(split_ops, 1U), 2U);
    top->append(split);

    // a region op consuming an OUTER value + an inner op whose STRING attr contains a brace.
    Value*           reg_ops[1] = {r};
    Operation* const rop =
        ctx.create_operation(ctx.intern_op("scf", "region"), ConstSpan<Value*>(reg_ops, 1U), 0U, {}, 1U);
    top->append(rop);
    Block* const inner = ctx.create_block(1U, TypeId{1U});
    rop->region(0)->append(inner);
    Value*           use_ops[2] = {r, inner->arg(0U)}; // consumes the outer %r AND the inner block arg
    Operation* const use = ctx.create_operation(ctx.intern_op("test", "use"), ConstSpan<Value*>(use_ops, 2U), 0U);
    ctx.set_attr(use, "note", ctx.attr_string("has a { brace")); // a brace INSIDE a nested op's string attr
    inner->append(use);

    // an EMPTY region op (num_regions=1, no block)
    Operation* const eop = ctx.create_operation(ctx.intern_op("test", "empty"), {}, 0U, {}, 1U);
    top->append(eop);

    // a MULTI-BLOCK region
    Operation* const mb = ctx.create_operation(ctx.intern_op("cf", "multiblock"), {}, 0U, {}, 1U);
    top->append(mb);
    Block* const b1 = ctx.create_block(0U);
    Block* const b2 = ctx.create_block(1U, TypeId{3U});
    mb->region(0)->append(b1);
    mb->region(0)->append(b2);
    b1->append(ctx.create_operation(ctx.intern_op("cf", "br"), {}, 0U));
    Value* sink_ops[1] = {b2->arg(0U)};
    b2->append(ctx.create_operation(ctx.intern_op("test", "sink"), ConstSpan<Value*>(sink_ops, 1U), 0U));

    // an UNREGISTERED-dialect op (opaque)
    top->append(ctx.create_operation(ctx.intern_op("plugin", "widget"), {}, 0U));

    // use-before-def in a Graph region: consumer appended BEFORE producer -> the operand references an id defined
    // LATER in the walk -> exercises the fixup pass in BOTH loaders.
    Operation* const producer   = ctx.create_operation(ctx.intern_op("test", "producer"), {}, 1U, TypeId{1U});
    Value*           con_ops[1] = {producer->result(0U)};
    Operation* const consumer = ctx.create_operation(ctx.intern_op("test", "consumer"), ConstSpan<Value*>(con_ops, 1U), 0U);
    top->append(consumer); // appended first...
    top->append(producer); // ...defined after

    // func.func + return (a nested function) and a func.call to it (a SymbolRef attr).
    Operation* const fn = func::create_func(ctx, *m, "callee_fn", Visibility::Public, 1U, TypeId{1U});
    top->append(fn);
    Block* const     fb         = func::func_body_block(fn);
    Value*           ret_ops[1] = {fb->arg(0U)};
    fb->append(func::create_return(ctx, ConstSpan<Value*>(ret_ops, 1U)));

    Value*           call_args[1] = {a0};
    Operation* const call = func::create_call(ctx, "callee_fn", ConstSpan<Value*>(call_args, 1U), 1U, TypeId{1U});
    top->append(call);

    return m;
}
} // namespace crd::ceir::test
