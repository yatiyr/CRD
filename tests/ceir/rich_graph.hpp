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
    using crd::containers::StringView;

    // A spread of interned types (CEIR-3a): scalars, a NESTED aggregate carried as a value type, and a NAMED struct
    // carried as a type attribute — so the binary TYPE pool exercises child-first ordering + STRP name/label refs.
    const TypeId     i32      = ctx.type_i32();
    const TypeId     f32      = ctx.type_f32();
    const TypeId     vec4f    = ctx.type_vector(f32, 4U);
    const TypeId     ftys[2]  = {i32, f32};
    const StringView fnames[2] = {StringView("x"), StringView("y")};
    const TypeId     point    = ctx.type_struct("Point", ConstSpan<TypeId>(ftys, 2U), ConstSpan<StringView>(fnames, 2U));
    // a GENERIC composite (CEIR-3b): fn<(T:Ord)->(option<i32>)> — puts param/trait/callable under the round-trip + purity gates
    const TypeId     ord      = ctx.type_trait("Ord", {});
    const TypeId     ordc[1]  = {ord};
    const TypeId     tparam   = ctx.type_param("T", ConstSpan<TypeId>(ordc, 1U));
    const TypeId     cps[1]   = {tparam};
    const TypeId     crs[1]   = {ctx.type_option(i32)};
    const TypeId     genfn    = ctx.type_callable(ConstSpan<TypeId>(cps, 1U), ConstSpan<TypeId>(crs, 1U));
    // a RESOURCE + VIEW composite (CEIR-3c): a view of a 2D image — puts resource/view under round-trip + purity
    const TypeId     img2d    = ctx.type_image(ImageDim::Dim2D, vec4f);
    const TypeId     imgview  = ctx.type_view(img2d, ViewRange::Mip | ViewRange::Layer);
    // a TENSOR with a symbolic dim (CEIR-3d) — the symbolic name rides STRP + the child-first pool
    const TypeId     tdims[3] = {ctx.type_dim_static(4U), ctx.type_dim_symbolic("N"), ctx.type_dim_dynamic()};
    const TypeId     tshape   = ctx.type_shape(ConstSpan<TypeId>(tdims, 3U));
    const TypeId     tenty    = ctx.type_tensor(f32, tshape);
    // a physical QUANTITY (CEIR-3e): a vec3 tagged with Length — the packed dimension rides count+cols
    QuantityDim      qlen;
    qlen.exp[0] = static_cast<crd::i8>(1);
    const TypeId qvec3len = ctx.type_quantity(ctx.type_vector(f32, 3U), qlen);
    // an OWNERSHIP-QUALIFIED type (CEIR-3f): a borrowed view of a plain buffer — the ownership kind rides count
    const TypeId borrowbuf = ctx.type_qualified(OwnershipKind::BorrowedView, ctx.type_buffer(BufferMode::Plain, f32));

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(1U, i32); // ^bb0(%0 : !i32)
    m->body()->append(top);
    Value* const a0 = top->arg(0U);

    // %r = test.add(%0, %0) : !vec<4x!f32>  with an attribute of EVERY kind (set out of name order on purpose).
    Value*           add_ops[2] = {a0, a0};
    Operation* const add =
        ctx.create_operation(ctx.intern_op("test", "add"), ConstSpan<Value*>(add_ops, 2U), 1U, vec4f);
    ctx.set_attr(add, "i", ctx.attr_int(-42));                     // negative int
    ctx.set_attr(add, "frac", ctx.attr_float(4.5));
    ctx.set_attr(add, "whole", ctx.attr_float(4.0));               // must survive as "4.0", re-read as float
    ctx.set_attr(add, "big", ctx.attr_float(1.0e20));             // exponent float
    ctx.set_attr(add, "flag", ctx.attr_bool(true));
    ctx.set_attr(add, "s", ctx.attr_string(R"(he said "hi"\n)")); // embedded quote + backslash (raw literal)
    ctx.set_attr(add, "sym", ctx.attr_symbol("target"));
    ctx.set_attr(add, "ty", ctx.attr_type(point));   // a NAMED struct type as an attribute value
    ctx.set_attr(add, "ty2", ctx.attr_type(genfn));   // a GENERIC callable type (param/trait/callable coverage)
    ctx.set_attr(add, "ty3", ctx.attr_type(imgview)); // a RESOURCE VIEW type (image/view coverage)
    ctx.set_attr(add, "ty4", ctx.attr_type(tenty));    // a TENSOR with a symbolic shape dim (dim/shape/tensor coverage)
    ctx.set_attr(add, "ty5", ctx.attr_type(qvec3len)); // a physical QUANTITY (vec3 Length) — dimension pack coverage
    ctx.set_attr(add, "ty6", ctx.attr_type(borrowbuf)); // an OWNERSHIP-QUALIFIED type (borrowed buffer) — qual coverage
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
    Block* const inner = ctx.create_block(1U, i32);
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
    Block* const b2 = ctx.create_block(2U, f32); // 2 args (uniform type) — exercises the v2 per-block-arg N>=2 path
    mb->region(0)->append(b1);
    mb->region(0)->append(b2);
    b1->append(ctx.create_operation(ctx.intern_op("cf", "br"), {}, 0U));
    Value* sink_ops[1] = {b2->arg(0U)};
    b2->append(ctx.create_operation(ctx.intern_op("test", "sink"), ConstSpan<Value*>(sink_ops, 1U), 0U));

    // an UNREGISTERED-dialect op (opaque)
    top->append(ctx.create_operation(ctx.intern_op("plugin", "widget"), {}, 0U));

    // use-before-def in a Graph region: consumer appended BEFORE producer -> the operand references an id defined
    // LATER in the walk -> exercises the fixup pass in BOTH loaders.
    Operation* const producer   = ctx.create_operation(ctx.intern_op("test", "producer"), {}, 1U, i32);
    Value*           con_ops[1] = {producer->result(0U)};
    Operation* const consumer = ctx.create_operation(ctx.intern_op("test", "consumer"), ConstSpan<Value*>(con_ops, 1U), 0U);
    top->append(consumer); // appended first...
    top->append(producer); // ...defined after

    // func.func + return (a nested function) and a func.call to it (a SymbolRef attr).
    Operation* const fn = func::create_func(ctx, *m, "callee_fn", Visibility::Public, 1U, i32);
    top->append(fn);
    Block* const     fb         = func::func_body_block(fn);
    Value*           ret_ops[1] = {fb->arg(0U)};
    fb->append(func::create_return(ctx, ConstSpan<Value*>(ret_ops, 1U)));

    Value*           call_args[1] = {a0};
    Operation* const call = func::create_call(ctx, "callee_fn", ConstSpan<Value*>(call_args, 1U), 1U, i32);
    top->append(call);

    return m;
}
} // namespace crd::ceir::test
