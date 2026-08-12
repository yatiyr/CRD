#pragma once

// ceir_render_triangle.hpp — CEIR-14z-3: a SHARED, backend-neutral CEIR RENDER program + the render EXECUTOR wrapper for
// the render DEVICE pixel proof (D-007, Option A test-surface). A CEIR
//   func main { render.scope(color_attachment{load=clear, clear=(0,0,1,1) BLUE}) { render.draw(3,1) program=@tri } }
// is built ONCE here, lowered by `lower_region`, and driven through `execute_render_lowered` onto a REAL
// `ICommandEncoder`. The SAME CEIR program feeds Vulkan (tests/ceir-gpu-vulkan) and DX12 (tests/ceir-gpu-dx12), so ONE
// CEIR render lowers to EVERY backend (ADR-0101 / ADR-0126). The triangle GEOMETRY is the shared CKIR triangle
// (crd::gputest::build_triangle_vs/fs) — this header only builds the CEIR wrapper + owns the lower→execute wiring.
//
// ⛔ Catch2-FREE (like ckir_raster_triangle.hpp): the per-backend test owns the device rig, the WARN-skips, and the pixel
// asserts — so "skip" (no shader tier) never gets conflated with "fail" (a resolved program that didn't render). The
// materializers only STORE the resolved target/program pointers (never deref), so identity-sentinel resolvers suffice
// (the caller passes the REAL device object as `user`, we round-trip it — no fake subclass), exactly as the 14z-2 unit.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/lower.hpp>            // lower_region / LoweredCommand
#include <crd/ceir/gpu/render_materialize.hpp> // execute_render_lowered / ExecuteError / the resolver typedefs
#include <crd/ceir/render.hpp>

#include <crd/gpu/raster_context.hpp> // IRasterTarget / IRasterProgram / ICommandEncoder

#include <crd/memory/allocator.hpp>

namespace crd::ceir_gpu_test
{
namespace ce = crd::ceir;

// The built CEIR render program: the `module` (for find_render_misuse) + the func-body `body` block (for lower_region).
struct CeirRenderProgram
{
    ce::Module* module = nullptr;
    ce::Block*  body   = nullptr;
};

// A fresh module with `func main {}`; returns its body block (where the render.scopes go) + the module via `out_module`.
// Registers the arith/func/resource/render dialects (idempotent per context).
inline ce::Block* ceir_fresh_render_module(ce::Context& ctx, ce::Module*& out_module)
{
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::render::register_dialect(ctx); // the ops + effects (lower_region reads the GPUCommand effect) + type-classes
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m->body()->append(top);
    }
    ce::Operation* const fn = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(fn);
    out_module = m;
    return ce::func::func_body_block(fn);
}

// Append ONE `render.scope(color_attachment(new img){load=clear, clear=(cr,cg,cb,1)}) { render.draw(3,1) @tri }` to `b`
// (a procedural triangle: the VS reads gl_VertexIndex, 0 bindings ⇒ GeometryKind::None). ⭐ clear_a=1.0 is explicit — the
// materializer defaults absent clear channels to 0. Returns the color_attachment op so a per-op target resolver can map it
// → a device target (⭐ the SAME map serves BOTH a multi-SCOPE program and a multi-ATTACHMENT MRT scope).
inline ce::Operation* ceir_append_triangle_scope(ce::Context& ctx, ce::Block* b, crd::u32 dim, double cr, double cg, double cb)
{
    const ce::OpId   decl_id  = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id   = ctx.intern_op("render", "color_attachment");
    const ce::OpId   scope_id = ctx.intern_op("render", "scope");
    const ce::OpId   draw_id  = ctx.intern_op("render", "draw");
    const ce::OpId   const_id = ctx.intern_op("arith", "const");
    const ce::TypeId f32      = ctx.type_f32();

    ce::Operation* const dimg = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(dimg);
    ce::Value*           imgv[1] = {dimg->result(0U)};
    ce::Operation* const ca      = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(imgv, 1U), 1U,
                                                        ce::render::type_color_attachment(ctx, dimg->result(0U)->type()));
    ctx.set_attr(ca, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(ca, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
    ctx.set_attr(ca, "clear_r", ctx.attr_float(cr));
    ctx.set_attr(ca, "clear_g", ctx.attr_float(cg));
    ctx.set_attr(ca, "clear_b", ctx.attr_float(cb));
    ctx.set_attr(ca, "clear_a", ctx.attr_float(1.0));
    b->append(ca);

    ce::Value*           atts[1] = {ca->result(0U)};
    ce::Operation* const sc = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(sc, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc);

    ce::Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    ce::Operation* const vc = ctx.create_operation(const_id, {}, 1U, ctx.type_index());
    ctx.set_attr(vc, "value", ctx.attr_int(3));
    rb->append(vc);
    ce::Operation* const ic = ctx.create_operation(const_id, {}, 1U, ctx.type_index());
    ctx.set_attr(ic, "value", ctx.attr_int(1));
    rb->append(ic);
    ce::Value*           dops[2] = {vc->result(0U), ic->result(0U)};
    ce::Operation* const dr      = ctx.create_operation(draw_id, crd::containers::ConstSpan<ce::Value*>(dops, 2U), 0U);
    ctx.set_attr(dr, "program", ctx.attr_symbol(crd::containers::StringView("tri")));
    ctx.set_attr(dr, "access", ctx.attr_string(crd::containers::StringView("")));
    rb->append(dr);
    return ca;
}

// Build `func main { render.scope(color_attachment(img){clear=(0,0,1,1) BLUE}) { render.draw(3,1) @tri } }` in a fresh module.
inline CeirRenderProgram build_ceir_triangle_render(ce::Context& ctx, crd::u32 dim)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);
    (void)ceir_append_triangle_scope(ctx, b, dim, 0.0, 0.0, 1.0); // one scope, BLUE clear
    return CeirRenderProgram{m, b};
}

// CEIR-14z-4c: a TWO-SCOPE program — `func main { scope0(colorA{BLUE}){draw} scope1(colorB{GREEN}){draw} }`. Exercises
// execute_render_frame's N-SCOPE path (the reserve-then-push closure array = the 11b MIRROR scar shape, the scope-splitting
// walk, TWO frame-graph passes into TWO different targets). `att0`/`att1` are the two attachment ops for a CeirTargetMap.
struct CeirTwoScopeProgram
{
    ce::Module*    module = nullptr;
    ce::Block*     body   = nullptr;
    ce::Operation* att0   = nullptr; // scope 0 color_attachment (BLUE clear)
    ce::Operation* att1   = nullptr; // scope 1 color_attachment (GREEN clear)
};
inline CeirTwoScopeProgram build_ceir_two_scope_render(ce::Context& ctx, crd::u32 dim)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);
    ce::Operation* const a0 = ceir_append_triangle_scope(ctx, b, dim, 0.0, 0.0, 1.0); // scope 0: BLUE
    ce::Operation* const a1 = ceir_append_triangle_scope(ctx, b, dim, 0.0, 1.0, 0.0); // scope 1: GREEN
    return CeirTwoScopeProgram{m, b, a0, a1};
}

// CEIR-14z-4c: an MRT program — `func main { render.scope(color0{BLUE}, color1{RED}) { render.draw(3,1, %vbuf) @tri } }`.
// TWO color attachments with DISTINCT clears + a StoragePull draw binding a vertex buffer (operand 2 ⇒ the MRT arm
// draw_storage_mrt). Pairs with build_vertex_pull_vs + build_gbuffer_two_output_fs (writes RED@0 / GREEN@1). `att0`/`att1`
// are the two attachment ops (for a CeirTargetMap); the single `%vbuf` binding resolves via ceir_render_binding_resolver.
struct CeirMrtProgram
{
    ce::Module*    module = nullptr;
    ce::Block*     body   = nullptr;
    ce::Operation* att0   = nullptr; // color 0 (the FS writes RED here)
    ce::Operation* att1   = nullptr; // color 1 (the FS writes GREEN here)
};
// Append the storage buffer + a render.scope over {a0, a1} + a StoragePull `render.draw(3,1,%vbuf) @tri {access="r"}` to `b`
// — the MRT scope-body shared by the float (build_ceir_mrt_render) and uint (build_ceir_mrt_uint_render) programs.
inline void ceir_append_mrt_body(ce::Context& ctx, ce::Block* b, crd::u32 dim, ce::Operation* a0, ce::Operation* a1)
{
    const ce::OpId   decl_id  = ctx.intern_op("resource", "declare");
    const ce::OpId   scope_id = ctx.intern_op("render", "scope");
    const ce::OpId   draw_id  = ctx.intern_op("render", "draw");
    const ce::OpId   const_id = ctx.intern_op("arith", "const");
    const ce::TypeId f32      = ctx.type_f32();

    ce::Operation* const vbuf = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(vbuf);

    ce::Value*           atts[2] = {a0->result(0U), a1->result(0U)};
    ce::Operation* const sc = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts, 2U), 0U, {}, 1U);
    ctx.set_attr(sc, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc);

    ce::Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    ce::Operation* const vc = ctx.create_operation(const_id, {}, 1U, ctx.type_index());
    ctx.set_attr(vc, "value", ctx.attr_int(3));
    rb->append(vc);
    ce::Operation* const ic = ctx.create_operation(const_id, {}, 1U, ctx.type_index());
    ctx.set_attr(ic, "value", ctx.attr_int(1));
    rb->append(ic);
    ce::Value*           dops[3] = {vc->result(0U), ic->result(0U), vbuf->result(0U)};
    ce::Operation* const dr      = ctx.create_operation(draw_id, crd::containers::ConstSpan<ce::Value*>(dops, 3U), 0U);
    ctx.set_attr(dr, "program", ctx.attr_symbol(crd::containers::StringView("tri")));
    ctx.set_attr(dr, "access", ctx.attr_string(crd::containers::StringView("r"))); // one binding ⇒ one access token
    rb->append(dr);
}
inline CeirMrtProgram build_ceir_mrt_render(ce::Context& ctx, crd::u32 dim)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id  = ctx.intern_op("render", "color_attachment");
    const ce::TypeId f32     = ctx.type_f32();

    // a color attachment over a fresh f32 image, cleared to (cr,cg,cb,1).
    const auto make_att = [&](double cr, double cg, double cb) -> ce::Operation* {
        ce::Operation* const img = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
        b->append(img);
        ce::Value*           iv[1] = {img->result(0U)};
        ce::Operation* const ca    = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(iv, 1U), 1U,
                                                          ce::render::type_color_attachment(ctx, img->result(0U)->type()));
        ctx.set_attr(ca, "load", ctx.attr_string(crd::containers::StringView("clear")));
        ctx.set_attr(ca, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
        ctx.set_attr(ca, "clear_r", ctx.attr_float(cr));
        ctx.set_attr(ca, "clear_g", ctx.attr_float(cg));
        ctx.set_attr(ca, "clear_b", ctx.attr_float(cb));
        ctx.set_attr(ca, "clear_a", ctx.attr_float(1.0));
        b->append(ca);
        return ca;
    };
    ce::Operation* const a0 = make_att(0.0, 0.0, 1.0); // color 0: BLUE clear
    ce::Operation* const a1 = make_att(1.0, 0.0, 0.0); // color 1: RED clear
    ceir_append_mrt_body(ctx, b, dim, a0, a1);
    return CeirMrtProgram{m, b, a0, a1};
}

// CEIR-14z-4c(c2): a uint-HOMOGENEOUS MRT program — `func main { render.scope(color0{clear_uint=c0}, color1{clear_uint=c1})
// { render.draw(3,1, %vbuf) @tri } }` over TWO UINT (R32_UINT) attachments with DISTINCT TYPED (uint) clears. Proves the
// per-attachment UINT clear arm (14z-4b, implemented but unproven) — the corners read the distinct uint clears, and (with
// build_visbuffer_two_output_fs) the centres read the distinct uint outputs. ⛔ the image element is `type_int(32,false)`
// (unsigned Int), so the RAH-1a.1 `ClearKindFormatMismatch` verifier accepts the uint clear (uint clear ⇔ unsigned-Int format).
inline CeirMrtProgram build_ceir_mrt_uint_render(ce::Context& ctx, crd::u32 dim, crd::u32 clear0, crd::u32 clear1)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id  = ctx.intern_op("render", "color_attachment");
    const ce::TypeId u32     = ctx.type_int(32, false); // the unsigned-Int format the uint clear + R32_UINT target need

    const auto make_uint_att = [&](crd::u32 clear_val) -> ce::Operation* {
        ce::Operation* const img = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, u32));
        b->append(img);
        ce::Value*           iv[1] = {img->result(0U)};
        ce::Operation* const ca    = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(iv, 1U), 1U,
                                                          ce::render::type_color_attachment(ctx, img->result(0U)->type()));
        ctx.set_attr(ca, "load", ctx.attr_string(crd::containers::StringView("clear")));
        ctx.set_attr(ca, "clear_kind", ctx.attr_string(crd::containers::StringView("uint"))); // TYPED uint clear
        ctx.set_attr(ca, "clear_uint", ctx.attr_int(static_cast<crd::i64>(clear_val)));
        b->append(ca);
        return ca;
    };
    ce::Operation* const a0 = make_uint_att(clear0);
    ce::Operation* const a1 = make_uint_att(clear1);
    ceir_append_mrt_body(ctx, b, dim, a0, a1);
    return CeirMrtProgram{m, b, a0, a1};
}

// CEIR-14z-4c(c3): a HETEROGENEOUS (mixed-type) MRT program — color0 is a UINT (R32_UINT) attachment with a uint clear,
// color1 is a FLOAT (RGBA8) attachment with a float clear. The plainest reading of "typed clears asserted per-target":
// two attachments of DIFFERENT formats + different clear KINDS in one scope. ⛔ needs the DX12 pass_pso per-attachment-format
// fix (14z-4c c3 engine work); Vulkan is view-derived and handles it as-is. Pairs with build_gbuffer_uint_float_fs (id 7 @0,
// GREEN @1). `att0` = uint (clear `uint_clear`), `att1` = float (clear BLUE).
inline CeirMrtProgram build_ceir_mrt_mixed_render(ce::Context& ctx, crd::u32 dim, crd::u32 uint_clear)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id  = ctx.intern_op("render", "color_attachment");
    const ce::TypeId u32     = ctx.type_int(32, false);
    const ce::TypeId f32     = ctx.type_f32();

    // color 0: a UINT attachment (R32_UINT), uint clear.
    ce::Operation* const uimg = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, u32));
    b->append(uimg);
    ce::Value*           uiv[1] = {uimg->result(0U)};
    ce::Operation* const a0     = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(uiv, 1U), 1U,
                                                       ce::render::type_color_attachment(ctx, uimg->result(0U)->type()));
    ctx.set_attr(a0, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(a0, "clear_kind", ctx.attr_string(crd::containers::StringView("uint")));
    ctx.set_attr(a0, "clear_uint", ctx.attr_int(static_cast<crd::i64>(uint_clear)));
    b->append(a0);

    // color 1: a FLOAT attachment (RGBA8), BLUE clear.
    ce::Operation* const fimg = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(fimg);
    ce::Value*           fiv[1] = {fimg->result(0U)};
    ce::Operation* const a1     = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(fiv, 1U), 1U,
                                                       ce::render::type_color_attachment(ctx, fimg->result(0U)->type()));
    ctx.set_attr(a1, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(a1, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
    ctx.set_attr(a1, "clear_b", ctx.attr_float(1.0));
    ctx.set_attr(a1, "clear_a", ctx.attr_float(1.0));
    b->append(a1);

    ceir_append_mrt_body(ctx, b, dim, a0, a1);
    return CeirMrtProgram{m, b, a0, a1};
}

// CEIR-14z-5: a DEPTH-ONLY OCCLUSION program — TWO scopes sharing ONE color+depth device target T, proving a depth-only pass
// renders real depth ON DEVICE via ATTACHMENT-ONLY observation (no sampling — the advisor's gold-standard depth proof).
//   scope 0: render.scope(depth_attachment{clear_depth=1.0}) { render.draw(3,1,%vbuf) @depth }
//            — a StoragePull triangle with build_depth_only_const_fs(0.5) (n_out=0: the ⛔ depth-only-≠-forward scar honored
//              by construction) ⇒ writes depth 0.5 under the triangle, clear 1.0 elsewhere.
//   scope 1: render.scope(color_attachment{clear BLUE}, depth_attachment{load=LOAD}) { render.draw(3,1) @cd }
//            — a fullscreen (None geometry) draw, build_color_depth_fs(RED, frag_depth=0.75), depth-test LessEqual against the
//              LOADED scope-0 depth. Read back T's COLOR: centre = BLUE (0.75 > 0.5 FAILS), corner = RED (0.75 <= 1.0 PASSES).
// draw0 uses @depth, draw1 uses @cd ⇒ a per-op CeirProgramMap. All 3 attachments resolve to the SAME target T (the identity
// sentinel target resolver), which is why T must be a create_color_depth_target (has_depth() ⇒ draw_storage_depth_only accepts it).
struct CeirDepthProgram
{
    ce::Module*    module = nullptr;
    ce::Block*     body   = nullptr;
    ce::Operation* draw0  = nullptr; // scope 0 draw (the depth-only program)
    ce::Operation* draw1  = nullptr; // scope 1 draw (the color+depth program)
};
inline CeirDepthProgram build_ceir_depth_occlusion_render(ce::Context& ctx, crd::u32 dim)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id  = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id   = ctx.intern_op("render", "color_attachment");
    const ce::OpId   dep_id   = ctx.intern_op("render", "depth_attachment");
    const ce::OpId   scope_id = ctx.intern_op("render", "scope");
    const ce::OpId   draw_id  = ctx.intern_op("render", "draw");
    const ce::OpId   const_id = ctx.intern_op("arith", "const");
    const ce::TypeId f32      = ctx.type_f32();

    const auto index_const = [&](ce::Block* rb, crd::i64 v) -> ce::Operation* {
        ce::Operation* const c = ctx.create_operation(const_id, {}, 1U, ctx.type_index());
        ctx.set_attr(c, "value", ctx.attr_int(v));
        rb->append(c);
        return c;
    };

    // Shared vertex storage buffer (scope 0's StoragePull triangle binds it).
    ce::Operation* const vbuf = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(vbuf);

    // ── scope 0: DEPTH-ONLY. depth_attachment(clear_depth=1.0) over a fresh depth image; a StoragePull triangle draw. ──
    ce::Operation* const dimg0 = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(dimg0);
    ce::Value*           d0v[1] = {dimg0->result(0U)};
    ce::Operation* const dep0   = ctx.create_operation(dep_id, crd::containers::ConstSpan<ce::Value*>(d0v, 1U), 1U,
                                                       ce::render::type_depth_attachment(ctx, dimg0->result(0U)->type()));
    ctx.set_attr(dep0, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(dep0, "clear_depth", ctx.attr_float(1.0)); // compare absent ⇒ LessEqual default
    b->append(dep0);

    ce::Value*           atts0[1] = {dep0->result(0U)};
    ce::Operation* const sc0 = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts0, 1U), 0U, {}, 1U);
    ctx.set_attr(sc0, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc0, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc0);
    ce::Block* const rb0 = ctx.create_block(0U);
    sc0->region(0)->append(rb0);
    ce::Operation* const vc0     = index_const(rb0, 3);
    ce::Operation* const ic0     = index_const(rb0, 1);
    ce::Value*           d0ops[3] = {vc0->result(0U), ic0->result(0U), vbuf->result(0U)};
    ce::Operation* const draw0    = ctx.create_operation(draw_id, crd::containers::ConstSpan<ce::Value*>(d0ops, 3U), 0U);
    ctx.set_attr(draw0, "program", ctx.attr_symbol(crd::containers::StringView("depth")));
    ctx.set_attr(draw0, "access", ctx.attr_string(crd::containers::StringView("r"))); // one binding ⇒ one access token
    rb0->append(draw0);

    // ── scope 1: COLOR (clear BLUE) + DEPTH (LOAD scope-0's depth, LessEqual). A fullscreen (None geometry) draw. ──
    ce::Operation* const cimg = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(cimg);
    ce::Value*           civ[1] = {cimg->result(0U)};
    ce::Operation* const col1   = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(civ, 1U), 1U,
                                                       ce::render::type_color_attachment(ctx, cimg->result(0U)->type()));
    ctx.set_attr(col1, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(col1, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
    ctx.set_attr(col1, "clear_b", ctx.attr_float(1.0)); // BLUE
    ctx.set_attr(col1, "clear_a", ctx.attr_float(1.0));
    b->append(col1);

    ce::Operation* const dimg1 = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(dimg1);
    ce::Value*           d1v[1] = {dimg1->result(0U)};
    ce::Operation* const dep1   = ctx.create_operation(dep_id, crd::containers::ConstSpan<ce::Value*>(d1v, 1U), 1U,
                                                       ce::render::type_depth_attachment(ctx, dimg1->result(0U)->type()));
    ctx.set_attr(dep1, "load", ctx.attr_string(crd::containers::StringView("load"))); // ⭐ LOAD scope-0's depth (do NOT re-clear)
    b->append(dep1);

    ce::Value*           atts1[2] = {col1->result(0U), dep1->result(0U)};
    ce::Operation* const sc1 = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts1, 2U), 0U, {}, 1U);
    ctx.set_attr(sc1, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc1, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc1);
    ce::Block* const rb1 = ctx.create_block(0U);
    sc1->region(0)->append(rb1);
    ce::Operation* const vc1     = index_const(rb1, 3);
    ce::Operation* const ic1     = index_const(rb1, 1);
    ce::Value*           d1ops[2] = {vc1->result(0U), ic1->result(0U)}; // 2 operands ⇒ None geometry (fullscreen)
    ce::Operation* const draw1    = ctx.create_operation(draw_id, crd::containers::ConstSpan<ce::Value*>(d1ops, 2U), 0U);
    ctx.set_attr(draw1, "program", ctx.attr_symbol(crd::containers::StringView("cd")));
    ctx.set_attr(draw1, "access", ctx.attr_string(crd::containers::StringView("")));
    rb1->append(draw1);

    return CeirDepthProgram{m, b, draw0, draw1};
}

// CEIR-14z-6: an INDEXED-INDIRECT program — `func main { render.scope(color{BLUE}) { render.draw_indirect(%args, %vbuf)
// {program=@di, access="r", max_draws=2, index_offset} } }`. Operand 0 = %args (the indirect-args buffer, resolved to
// GeometrySource.args_buffer), operand 1 = %vbuf (the pull+index buffer, the ONE binding). The engine's indirect draw is
// INDEXED (Nanite-style): %vbuf holds the verts AND the index section at `index_offset`. Pairs with build_vertex_pull_drawindex_vs
// (X-shift by the pushed SV_DrawIndex) + a 2-command args buffer ⇒ draw 0 → left, draw 1 → right (the DrawIndex-push proof).
// `att`/`args`/`vbuf` feed the resolvers: `att` → the target (identity sentinel), {args,vbuf}->result(0) → a CeirBufferMap.
struct CeirIndirectProgram
{
    ce::Module*    module = nullptr;
    ce::Block*     body   = nullptr;
    ce::Operation* att    = nullptr; // the color attachment (→ the render target)
    ce::Operation* args   = nullptr; // %args decl (operand 0 → GeometrySource.args_buffer)
    ce::Operation* vbuf   = nullptr; // %vbuf decl (operand 1 → the pull+index binding)
};
inline CeirIndirectProgram build_ceir_draw_indirect_render(ce::Context& ctx, crd::u32 dim, crd::u32 index_offset)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id  = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id   = ctx.intern_op("render", "color_attachment");
    const ce::OpId   scope_id = ctx.intern_op("render", "scope");
    const ce::OpId   draw_id  = ctx.intern_op("render", "draw_indirect");
    const ce::TypeId f32      = ctx.type_f32();

    ce::Operation* const args = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(args);
    ce::Operation* const vbuf = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(vbuf);

    ce::Operation* const img = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(img);
    ce::Value*           iv[1] = {img->result(0U)};
    ce::Operation* const ca    = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(iv, 1U), 1U,
                                                      ce::render::type_color_attachment(ctx, img->result(0U)->type()));
    ctx.set_attr(ca, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(ca, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
    ctx.set_attr(ca, "clear_b", ctx.attr_float(1.0)); // BLUE
    ctx.set_attr(ca, "clear_a", ctx.attr_float(1.0));
    b->append(ca);

    ce::Value*           atts[1] = {ca->result(0U)};
    ce::Operation* const sc = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(sc, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc);

    ce::Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    ce::Value*           dops[2] = {args->result(0U), vbuf->result(0U)}; // %args (0), %vbuf (1 — the binding tail)
    ce::Operation* const dr      = ctx.create_operation(draw_id, crd::containers::ConstSpan<ce::Value*>(dops, 2U), 0U);
    ctx.set_attr(dr, "program", ctx.attr_symbol(crd::containers::StringView("di")));
    ctx.set_attr(dr, "access", ctx.attr_string(crd::containers::StringView("r"))); // one binding (%vbuf) ⇒ one access token
    ctx.set_attr(dr, "max_draws", ctx.attr_int(2));                                // TWO sub-draws ⇒ DrawIndex 0 and 1
    ctx.set_attr(dr, "index_offset", ctx.attr_int(static_cast<crd::i64>(index_offset)));
    rb->append(dr);

    return CeirIndirectProgram{m, b, ca, args, vbuf};
}

// CEIR-14z-6(c2): an INDEXED-INDIRECT-COUNT program — `render.draw_indirect_count(%args, %count, %vbuf) {max_draws=2, ...}`.
// Adds the device %count buffer (operand 1): the GPU reads the ACTUAL draw count from it, bounded by max_draws. The proof
// uploads count=1 ⇒ only sub-draw 0 runs ⇒ the RIGHT half reads the clear (the device count GATES execution, not just plumbed).
// Operand order: %args(0), %count(1), %vbuf(2 — the ONE binding). `count` feeds GeometrySource.count_buffer.
struct CeirIndirectCountProgram
{
    ce::Module*    module = nullptr;
    ce::Block*     body   = nullptr;
    ce::Operation* att    = nullptr; // the color attachment (→ the render target)
    ce::Operation* args   = nullptr; // %args decl (operand 0 → GeometrySource.args_buffer)
    ce::Operation* count  = nullptr; // %count decl (operand 1 → GeometrySource.count_buffer)
    ce::Operation* vbuf   = nullptr; // %vbuf decl (operand 2 → the pull+index binding)
};
inline CeirIndirectCountProgram build_ceir_draw_indirect_count_render(ce::Context& ctx, crd::u32 dim, crd::u32 index_offset)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id  = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id   = ctx.intern_op("render", "color_attachment");
    const ce::OpId   scope_id = ctx.intern_op("render", "scope");
    const ce::OpId   draw_id  = ctx.intern_op("render", "draw_indirect_count");
    const ce::TypeId f32      = ctx.type_f32();

    ce::Operation* const args = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(args);
    ce::Operation* const count = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(count);
    ce::Operation* const vbuf = ctx.create_operation(decl_id, {}, 1U, ctx.type_buffer(ce::BufferMode::Plain, f32));
    b->append(vbuf);

    ce::Operation* const img = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(img);
    ce::Value*           iv[1] = {img->result(0U)};
    ce::Operation* const ca    = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(iv, 1U), 1U,
                                                      ce::render::type_color_attachment(ctx, img->result(0U)->type()));
    ctx.set_attr(ca, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(ca, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
    ctx.set_attr(ca, "clear_b", ctx.attr_float(1.0)); // BLUE
    ctx.set_attr(ca, "clear_a", ctx.attr_float(1.0));
    b->append(ca);

    ce::Value*           atts[1] = {ca->result(0U)};
    ce::Operation* const sc = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(sc, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc);

    ce::Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    ce::Value*           dops[3] = {args->result(0U), count->result(0U), vbuf->result(0U)}; // %args(0), %count(1), %vbuf(2)
    ce::Operation* const dr      = ctx.create_operation(draw_id, crd::containers::ConstSpan<ce::Value*>(dops, 3U), 0U);
    ctx.set_attr(dr, "program", ctx.attr_symbol(crd::containers::StringView("di")));
    ctx.set_attr(dr, "access", ctx.attr_string(crd::containers::StringView("r"))); // one binding (%vbuf) ⇒ one access token
    ctx.set_attr(dr, "max_draws", ctx.attr_int(2));
    ctx.set_attr(dr, "index_offset", ctx.attr_int(static_cast<crd::i64>(index_offset)));
    rb->append(dr);

    return CeirIndirectCountProgram{m, b, ca, args, count, vbuf};
}

// CEIR-14z-7: a MESH-DISPATCH program — `func main { render.scope(color{BLUE}) { render.mesh_dispatch(gx,gy,gz) {program=@mesh} } }`.
// A PROCEDURAL mesh dispatch (no bindings ⇒ GeometryKind::Meshlet, buf=null ⇒ draw_mesh). Pairs with build_triangle_mesh (the
// mesh KIR entry) + build_triangle_fs; grid (1,1,1) ⇒ ONE meshlet workgroup ⇒ the shared triangle (centre RED / corner BLUE).
// ⛔ the mesh program is created via create_mesh_program (NOT create_raster_program). Grid is parameterized: (2,2,1) etc.
// exercises the LOUD y/z-narrowing refusal (the Meshlet verb is 1D — a y/z != 1 grid ⇒ materialize fails ⇒ UnsupportedCommand).
struct CeirMeshProgram
{
    ce::Module*    module = nullptr;
    ce::Block*     body   = nullptr;
    ce::Operation* att    = nullptr; // the color attachment (→ the render target, identity sentinel)
};
inline CeirMeshProgram build_ceir_mesh_dispatch_render(ce::Context& ctx, crd::u32 dim, crd::u32 gx, crd::u32 gy, crd::u32 gz)
{
    ce::Module* m = nullptr;
    ce::Block*  b = ceir_fresh_render_module(ctx, m);

    const ce::OpId   decl_id  = ctx.intern_op("resource", "declare");
    const ce::OpId   col_id   = ctx.intern_op("render", "color_attachment");
    const ce::OpId   scope_id = ctx.intern_op("render", "scope");
    const ce::OpId   mesh_id  = ctx.intern_op("render", "mesh_dispatch");
    const ce::OpId   const_id = ctx.intern_op("arith", "const");
    const ce::TypeId f32      = ctx.type_f32();

    ce::Operation* const img = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
    b->append(img);
    ce::Value*           iv[1] = {img->result(0U)};
    ce::Operation* const ca    = ctx.create_operation(col_id, crd::containers::ConstSpan<ce::Value*>(iv, 1U), 1U,
                                                      ce::render::type_color_attachment(ctx, img->result(0U)->type()));
    ctx.set_attr(ca, "load", ctx.attr_string(crd::containers::StringView("clear")));
    ctx.set_attr(ca, "clear_kind", ctx.attr_string(crd::containers::StringView("float")));
    ctx.set_attr(ca, "clear_b", ctx.attr_float(1.0)); // BLUE
    ctx.set_attr(ca, "clear_a", ctx.attr_float(1.0));
    b->append(ca);

    ce::Value*           atts[1] = {ca->result(0U)};
    ce::Operation* const sc = ctx.create_operation(scope_id, crd::containers::ConstSpan<ce::Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(sc, "width", ctx.attr_int(static_cast<crd::i64>(dim)));
    ctx.set_attr(sc, "height", ctx.attr_int(static_cast<crd::i64>(dim)));
    b->append(sc);

    ce::Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    const auto grid = [&](crd::u32 v) -> ce::Operation* {
        ce::Operation* const c = ctx.create_operation(const_id, {}, 1U, ctx.type_index());
        ctx.set_attr(c, "value", ctx.attr_int(static_cast<crd::i64>(v)));
        rb->append(c);
        return c;
    };
    ce::Operation* const cx      = grid(gx);
    ce::Operation* const cy      = grid(gy);
    ce::Operation* const cz      = grid(gz);
    ce::Value*           dops[3] = {cx->result(0U), cy->result(0U), cz->result(0U)};
    ce::Operation* const dr      = ctx.create_operation(mesh_id, crd::containers::ConstSpan<ce::Value*>(dops, 3U), 0U);
    ctx.set_attr(dr, "program", ctx.attr_symbol(crd::containers::StringView("mesh")));
    ctx.set_attr(dr, "access", ctx.attr_string(crd::containers::StringView(""))); // zero bindings ⇒ empty access
    rb->append(dr);

    return CeirMeshProgram{m, b, ca};
}

// Identity-sentinel resolvers: the caller passes the REAL device object as `user`, we round-trip it (the materializers
// store the pointer, never deref it — no fake subclass, mirroring the 14z-2 device-free unit test).
inline crd::gpu::IRasterTarget* ceir_render_target_resolver(const ce::Operation*, void* user)
{
    return static_cast<crd::gpu::IRasterTarget*>(user);
}
inline crd::gpu::IRasterProgram* ceir_render_program_resolver(const ce::Operation*, void* user)
{
    return static_cast<crd::gpu::IRasterProgram*>(user);
}
// CEIR-14z-4c: the binding-operand resolver (identity sentinel — one storage buffer passed as `user`; the materializer
// stores the pointer, never derefs it). A multi-buffer draw would key by the operand `Value*`; the proofs bind ONE buffer.
inline crd::gpu::IStorageBuffer* ceir_render_binding_resolver(const ce::Value*, void* user)
{
    return static_cast<crd::gpu::IStorageBuffer*>(user);
}

// CEIR-14z-4c: a PER-OP target map — resolve a color/depth attachment op → its OWN device target (the identity sentinel
// returns ONE target for all ops, which cannot express a multi-SCOPE program or a multi-ATTACHMENT MRT scope). Pass a
// filled `CeirTargetMap` as the resolver's `user`.
struct CeirTargetMap
{
    const ce::Operation*     ops[8]     = {};
    crd::gpu::IRasterTarget* targets[8] = {};
    crd::u32                 n          = 0U;
    void add(const ce::Operation* op, crd::gpu::IRasterTarget* t)
    {
        ops[n]     = op;
        targets[n] = t;
        ++n;
    }
};
inline crd::gpu::IRasterTarget* ceir_mapped_target_resolver(const ce::Operation* op, void* user)
{
    auto* const m = static_cast<CeirTargetMap*>(user);
    for (crd::u32 i = 0; i < m->n; ++i)
    {
        if (m->ops[i] == op) { return m->targets[i]; }
    }
    return nullptr;
}

// CEIR-14z-5: a PER-OP program map — resolve a `render.draw` op → its OWN raster program (the identity sentinel returns ONE
// program for all draws, which cannot express two scopes running DIFFERENT programs, e.g. the depth-only occlusion proof's
// depth-only scope-0 program vs its color+depth scope-1 program). The twin of CeirTargetMap; pass a filled map as the
// program resolver's `user`. Keyed on the DRAW op (RasterProgramResolveFn receives the draw op, not an attachment).
struct CeirProgramMap
{
    const ce::Operation*      ops[8]      = {};
    crd::gpu::IRasterProgram* programs[8] = {};
    crd::u32                  n           = 0U;
    void add(const ce::Operation* op, crd::gpu::IRasterProgram* p)
    {
        ops[n]      = op;
        programs[n] = p;
        ++n;
    }
};
inline crd::gpu::IRasterProgram* ceir_mapped_program_resolver(const ce::Operation* op, void* user)
{
    auto* const m = static_cast<CeirProgramMap*>(user);
    for (crd::u32 i = 0; i < m->n; ++i)
    {
        if (m->ops[i] == op) { return m->programs[i]; }
    }
    return nullptr;
}

// CEIR-14z-6: a PER-OPERAND buffer map — resolve a draw's buffer operand (Value*) → its OWN storage buffer. The identity
// sentinel (ceir_render_binding_resolver) returns ONE buffer for ANY operand, which cannot express a draw binding DISTINCT
// buffers (indexed-indirect's %args vs its %vbuf). This is the FIRST resolver keyed on the operand — a sentinel that ignored
// its operand would now render the args as vertices (or vice-versa). Pass a filled CeirBufferMap as the resolver's `user`.
struct CeirBufferMap
{
    const ce::Value*          vals[8]    = {};
    crd::gpu::IStorageBuffer* buffers[8] = {};
    crd::u32                  n          = 0U;
    void add(const ce::Value* v, crd::gpu::IStorageBuffer* buf)
    {
        vals[n]    = v;
        buffers[n] = buf;
        ++n;
    }
};
inline crd::gpu::IStorageBuffer* ceir_mapped_binding_resolver(const ce::Value* v, void* user)
{
    auto* const m = static_cast<CeirBufferMap*>(user);
    for (crd::u32 i = 0; i < m->n; ++i)
    {
        if (m->vals[i] == v) { return m->buffers[i]; }
    }
    return nullptr;
}

// Lower `body` and drive the render-lowered command list through `enc` into `target`/`program` (the Option A
// test-surface executor). Returns the ExecuteError. ⛔ the caller must SCOPE `enc` so its submit+wait completes (the
// encoder's end_rendering/destructor) BEFORE reading pixels back from `target`.
[[nodiscard]] inline ce::gpu::ExecuteError run_ceir_render(const ce::Context& ctx, ce::Block& body,
                                                           crd::memory::IAllocator& alloc, crd::gpu::ICommandEncoder& enc,
                                                           crd::gpu::IRasterTarget* target, crd::gpu::IRasterProgram* program)
{
    crd::containers::Array<ce::gpu::LoweredCommand> cmds(&alloc);
    ce::gpu::lower_region(ctx, body, cmds);
    return ce::gpu::execute_render_lowered(ctx,
                                           crd::containers::ConstSpan<ce::gpu::LoweredCommand>(cmds.data(), cmds.size()),
                                           enc,
                                           ce::gpu::RenderResolvers{.target = ceir_render_target_resolver, .target_user = target,
                                                                    .program = ceir_render_program_resolver,
                                                                    .program_user = program});
}

// CEIR-14z-4a (the GOLD-STANDARD drive): lower `body` + drive the render program through the raster context's
// FRAME-RECORDING mode (execute_render_frame — one frame-graph pass per render.scope) instead of a standalone synchronous
// encoder. `raster` must expose create_frame_graph() (both real backends do). The frame graph owns the readback, so the
// caller reads pixels from `target` after this returns.
[[nodiscard]] inline ce::gpu::ExecuteError run_ceir_render_frame(const ce::Context& ctx, ce::Block& body,
                                                                crd::memory::IAllocator& alloc,
                                                                crd::gpu::IRasterContext& raster,
                                                                crd::gpu::IRasterTarget*  target,
                                                                crd::gpu::IRasterProgram* program)
{
    crd::containers::Array<ce::gpu::LoweredCommand> cmds(&alloc);
    ce::gpu::lower_region(ctx, body, cmds);
    return ce::gpu::execute_render_frame(ctx,
                                         crd::containers::ConstSpan<ce::gpu::LoweredCommand>(cmds.data(), cmds.size()),
                                         raster, alloc,
                                         ce::gpu::RenderResolvers{.target = ceir_render_target_resolver, .target_user = target,
                                                                  .program = ceir_render_program_resolver,
                                                                  .program_user = program});
}

// CEIR-14z-4c: the frame-recording drive with a PER-OP target map (for a multi-scope / multi-attachment program). `map`
// resolves each attachment op → its own target; `program` is shared by all draws. The frame graph owns the readback.
[[nodiscard]] inline ce::gpu::ExecuteError run_ceir_render_frame_mapped(const ce::Context& ctx, ce::Block& body,
                                                                        crd::memory::IAllocator& alloc,
                                                                        crd::gpu::IRasterContext& raster, CeirTargetMap& map,
                                                                        crd::gpu::IRasterProgram* program)
{
    crd::containers::Array<ce::gpu::LoweredCommand> cmds(&alloc);
    ce::gpu::lower_region(ctx, body, cmds);
    return ce::gpu::execute_render_frame(ctx,
                                         crd::containers::ConstSpan<ce::gpu::LoweredCommand>(cmds.data(), cmds.size()),
                                         raster, alloc,
                                         ce::gpu::RenderResolvers{.target = ceir_mapped_target_resolver, .target_user = &map,
                                                                  .program = ceir_render_program_resolver,
                                                                  .program_user = program});
}

// CEIR-14z-4c: the frame-recording drive for an MRT program — a per-op target `map` + a single storage `buffer` (bound via
// ceir_render_binding_resolver, so the draw's StoragePull binding tail resolves). Drives execute_render_frame with all three
// resolvers; the frame graph owns the readback (each target read back after this returns).
[[nodiscard]] inline ce::gpu::ExecuteError run_ceir_render_frame_mrt(const ce::Context& ctx, ce::Block& body,
                                                                     crd::memory::IAllocator& alloc,
                                                                     crd::gpu::IRasterContext& raster, CeirTargetMap& map,
                                                                     crd::gpu::IRasterProgram*  program,
                                                                     crd::gpu::IStorageBuffer*  buffer)
{
    crd::containers::Array<ce::gpu::LoweredCommand> cmds(&alloc);
    ce::gpu::lower_region(ctx, body, cmds);
    return ce::gpu::execute_render_frame(ctx,
                                         crd::containers::ConstSpan<ce::gpu::LoweredCommand>(cmds.data(), cmds.size()),
                                         raster, alloc,
                                         ce::gpu::RenderResolvers{.target = ceir_mapped_target_resolver, .target_user = &map,
                                                                  .program = ceir_render_program_resolver, .program_user = program,
                                                                  .storage = ceir_render_binding_resolver,
                                                                  .storage_user = buffer});
}

// CEIR-14z-5: the frame-recording drive for the DEPTH-ONLY occlusion program — the identity `target` (all 3 attachments share
// ONE color+depth T) + a per-op program `progs` map (scope 0's depth-only program vs scope 1's color+depth program) + a single
// storage `buffer` (scope 0's StoragePull triangle). Drives execute_render_frame with the mapped PROGRAM resolver; the frame
// graph owns the readback + the scope-0→scope-1 depth WAW ordering (both scopes write T). Read back T's COLOR after this returns.
[[nodiscard]] inline ce::gpu::ExecuteError run_ceir_render_frame_depth(const ce::Context& ctx, ce::Block& body,
                                                                       crd::memory::IAllocator&  alloc,
                                                                       crd::gpu::IRasterContext& raster,
                                                                       crd::gpu::IRasterTarget*  target, CeirProgramMap& progs,
                                                                       crd::gpu::IStorageBuffer* buffer)
{
    crd::containers::Array<ce::gpu::LoweredCommand> cmds(&alloc);
    ce::gpu::lower_region(ctx, body, cmds);
    return ce::gpu::execute_render_frame(ctx,
                                         crd::containers::ConstSpan<ce::gpu::LoweredCommand>(cmds.data(), cmds.size()),
                                         raster, alloc,
                                         ce::gpu::RenderResolvers{.target = ceir_render_target_resolver, .target_user = target,
                                                                  .program = ceir_mapped_program_resolver, .program_user = &progs,
                                                                  .storage = ceir_render_binding_resolver,
                                                                  .storage_user = buffer});
}

// CEIR-14z-6: the frame-recording drive for the INDEXED-INDIRECT program — the identity `target` + `program` (one scope, one
// draw) + a per-operand `buffers` map (the %args and %vbuf operands resolve to DISTINCT buffers — the first multi-buffer draw).
// The args/index/pull buffers are pre-uploaded (the 4c storage contract; NOT fg-tracked). The frame graph owns the readback.
[[nodiscard]] inline ce::gpu::ExecuteError run_ceir_render_frame_indirect(const ce::Context& ctx, ce::Block& body,
                                                                          crd::memory::IAllocator&  alloc,
                                                                          crd::gpu::IRasterContext& raster,
                                                                          crd::gpu::IRasterTarget*  target,
                                                                          crd::gpu::IRasterProgram* program,
                                                                          CeirBufferMap&            buffers)
{
    crd::containers::Array<ce::gpu::LoweredCommand> cmds(&alloc);
    ce::gpu::lower_region(ctx, body, cmds);
    return ce::gpu::execute_render_frame(ctx,
                                         crd::containers::ConstSpan<ce::gpu::LoweredCommand>(cmds.data(), cmds.size()),
                                         raster, alloc,
                                         ce::gpu::RenderResolvers{.target = ceir_render_target_resolver, .target_user = target,
                                                                  .program = ceir_render_program_resolver, .program_user = program,
                                                                  .storage = ceir_mapped_binding_resolver,
                                                                  .storage_user = &buffers});
}

} // namespace crd::ceir_gpu_test
