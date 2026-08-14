// CEIR-14a §40/§41 — the ceir.render dialect: render.scope (a REGION op) + render.color_attachment /
// render.depth_attachment DECLARE ops producing 8a attachment TYPES (the role rides the class). Context::find_render_misuse
// enforces the attachment/area contract the per-op structural verify cannot: attachment-operand-is-image, the closed-vocab
// per-attachment attrs (load/store/clear_kind/blend/compare), the RAH-1a.1 typed-clear-vs-format scar (a uint clear only on
// a uint-format target), every scope operand attachment-typed, at-most-one-depth (a TYPE property — color != depth,
// ADR-0111), the render area (w/h >= 1) + sample_count (a power of two in [1,64]). A scope round-trips text == builder
// (§121, no privileged path). ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp> // CEIR-14b: a compute.dispatch for the ComputeInRenderScope negative
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/render.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
struct Kit
{
    OpId col, dep, scope, decl, draw, drawi, cst, disp, dind, dindc, mesh, meshind;
    explicit Kit(Context& ctx)
        : col(ctx.intern_op("render", "color_attachment")), dep(ctx.intern_op("render", "depth_attachment")),
          scope(ctx.intern_op("render", "scope")), decl(ctx.intern_op("resource", "declare")),
          draw(ctx.intern_op("render", "draw")), drawi(ctx.intern_op("render", "draw_indexed")),
          cst(ctx.intern_op("arith", "const")), disp(ctx.intern_op("compute", "dispatch")),
          dind(ctx.intern_op("render", "draw_indirect")), dindc(ctx.intern_op("render", "draw_indirect_count")),
          mesh(ctx.intern_op("render", "mesh_dispatch")), meshind(ctx.intern_op("render", "mesh_dispatch_indirect"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)compute::register_compute_ops(ctx); // for the ComputeInRenderScope negative
        (void)render::register_dialect(ctx);      // the ops + the color/depth attachment type-classes
    }
};
// An index-typed constant (a draw count operand).
Value* mkindex(Context& ctx, const Kit& k, Block* b, i64 v)
{
    Operation* const c = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
    ctx.set_attr(c, "value", ctx.attr_int(v));
    b->append(c);
    return c->result(0U);
}
// A body BLOCK inside a scope's region (where the draws live). Returns the block.
Block* scope_body(Context& ctx, Operation* sc)
{
    Block* const rb = ctx.create_block(0U);
    sc->region(0)->append(rb);
    return rb;
}
// render.draw(%vcount, %icount, %binds...) {program, access} — appended to region block `rb`.
Operation* mkdraw(Context& ctx, const Kit& k, Block* rb, Value* vcount, Value* icount, ConstSpan<Value*> binds,
                  const char* prog, const char* access)
{
    Value* ops[2 + 8];
    ops[0] = vcount;
    ops[1] = icount;
    for (crd::usize i = 0; i < binds.size(); ++i) { ops[2 + i] = binds[i]; }
    Operation* const op = ctx.create_operation(k.draw, ConstSpan<Value*>(ops, 2U + binds.size()), 0U);
    ctx.set_attr(op, "program", ctx.attr_symbol(StringView(prog)));
    ctx.set_attr(op, "access", ctx.attr_string(StringView(access)));
    rb->append(op);
    return op;
}
// render.draw_indexed(%icount, %inst, %index_buffer, %binds...) {program, access}.
Operation* mkdrawi(Context& ctx, const Kit& k, Block* rb, Value* icount, Value* inst, Value* index_buffer,
                   ConstSpan<Value*> binds, const char* prog, const char* access)
{
    Value* ops[3 + 8];
    ops[0] = icount;
    ops[1] = inst;
    ops[2] = index_buffer;
    for (crd::usize i = 0; i < binds.size(); ++i) { ops[3 + i] = binds[i]; }
    Operation* const op = ctx.create_operation(k.drawi, ConstSpan<Value*>(ops, 3U + binds.size()), 0U);
    ctx.set_attr(op, "program", ctx.attr_symbol(StringView(prog)));
    ctx.set_attr(op, "access", ctx.attr_string(StringView(access)));
    rb->append(op);
    return op;
}
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m.body()->append(top);
    }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
// A graph-owned image resource of `fmt` format (its type is Image<2D, fmt>).
Value* mkimage(Context& ctx, const Kit& k, Block* b, TypeId fmt)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, fmt));
    b->append(d);
    return d->result(0U);
}
// A graph-owned buffer resource (a NON-image, for the AttachmentNotImage / ScopeOperandNotAttachment negatives).
Value* mkbuffer(Context& ctx, const Kit& k, Block* b)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
    b->append(d);
    return d->result(0U);
}
// A color attachment op over image value `img`; result type = the color-attachment type over `rt_image` (a valid image
// type — kept separate so the AttachmentNotImage negative can pass a buffer OPERAND while the RESULT type stays valid).
Operation* mkcolor(Context& ctx, const Kit& k, Block* b, Value* img, TypeId rt_image)
{
    Value* ops[1] = {img};
    Operation* const op = ctx.create_operation(k.col, ConstSpan<Value*>(ops, 1U), 1U, render::type_color_attachment(ctx, rt_image));
    b->append(op);
    return op;
}
Operation* mkdepth(Context& ctx, const Kit& k, Block* b, Value* img, TypeId rt_image)
{
    Value* ops[1] = {img};
    Operation* const op = ctx.create_operation(k.dep, ConstSpan<Value*>(ops, 1U), 1U, render::type_depth_attachment(ctx, rt_image));
    b->append(op);
    return op;
}
// A render.scope over the attachment values `atts`, with width/height (+ one graph region for the draws, empty at 14a).
Operation* mkscope(Context& ctx, const Kit& k, Block* b, ConstSpan<Value*> atts, i64 w, i64 h)
{
    Operation* const op = ctx.create_operation(k.scope, atts, 0U, {}, 1U);
    ctx.set_attr(op, "width", ctx.attr_int(w));
    ctx.set_attr(op, "height", ctx.attr_int(h));
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 14a: a well-formed render scope with color + depth attachments passes find_render_misuse", "[ceir][render]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m    = ctx.create_module();
    Block* const            b    = mkmain(ctx, *m);
    const TypeId            f32  = ctx.type_f32();
    Value* const            cimg = mkimage(ctx, k, b, f32);
    Value* const            dimg = mkimage(ctx, k, b, f32);
    Operation* const        col  = mkcolor(ctx, k, b, cimg, ctx.type_image(ImageDim::Dim2D, f32));
    ctx.set_attr(col, "load", ctx.attr_string("clear"));
    ctx.set_attr(col, "store", ctx.attr_string("store"));
    ctx.set_attr(col, "clear_kind", ctx.attr_string("float"));
    ctx.set_attr(col, "clear_r", ctx.attr_float(0.1));
    ctx.set_attr(col, "blend", ctx.attr_string("alpha"));
    Operation* const dep = mkdepth(ctx, k, b, dimg, ctx.type_image(ImageDim::Dim2D, f32));
    ctx.set_attr(dep, "load", ctx.attr_string("clear"));
    ctx.set_attr(dep, "clear_depth", ctx.attr_float(1.0));
    ctx.set_attr(dep, "compare", ctx.attr_string("less_equal"));
    ctx.set_attr(dep, "read_only", ctx.attr_bool(false));
    Value*           atts[2] = {col->result(0U), dep->result(0U)};
    Operation* const sc      = mkscope(ctx, k, b, ConstSpan<Value*>(atts, 2U), 1920, 1080);
    ctx.set_attr(sc, "sample_count", ctx.attr_int(4));
    CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::None);
}

TEST_CASE("ceir 14a: color and depth attachment TYPES are distinct (role-in-class, ADR-0111)", "[ceir][render]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    const TypeId            img = ctx.type_image(ImageDim::Dim2D, ctx.type_f32());
    const TypeId            c   = render::type_color_attachment(ctx, img);
    const TypeId            d   = render::type_depth_attachment(ctx, img);
    CHECK(c.valid());
    CHECK(d.valid());
    CHECK(c != d); // two type-classes with identical params are DIFFERENT TypeIds (the ADR-0111 landmine)
}

TEST_CASE("ceir 14a: a render scope round-trips text == builder (no privileged path, sec 121)", "[ceir][render]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m    = ctx.create_module();
    Block* const            b    = mkmain(ctx, *m);
    const TypeId            f32  = ctx.type_f32();
    Value* const            cimg = mkimage(ctx, k, b, f32);
    Operation* const        col  = mkcolor(ctx, k, b, cimg, ctx.type_image(ImageDim::Dim2D, f32));
    ctx.set_attr(col, "load", ctx.attr_string("clear"));
    Value*           atts[1] = {col->result(0U)};
    (void)mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 640, 480);

    const String txt = print(ctx, *m, &root);
    Context      ctx2(&root);
    const Kit    k2(ctx2); // the parse context needs the render dialect + its type-classes registered
    const ParseResult pr = parse(ctx2, StringView(txt.data(), txt.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String txt2 = print(ctx2, *pr.module, &root);
    CHECK(StringView(txt.data(), txt.size()) == StringView(txt2.data(), txt2.size())); // print(parse(print)) fixpoint
    CHECK(ctx2.find_render_misuse(*pr.module).kind == RenderMisuseKind::None);          // and it re-verifies clean
}

TEST_CASE("ceir 14a: find_render_misuse rejects every malformed render", "[ceir][render]")
{
    memory::MallocAllocator root;
    const TypeId            dummy; // built per-context below

    // AttachmentNotImage: a color attachment whose OPERAND is a buffer (result type stays a valid image attachment).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     buf = mkbuffer(ctx, k, b);
        (void)mkcolor(ctx, k, b, buf, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::AttachmentNotImage);
    }
    // LoadOpInvalid.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        ctx.set_attr(c, "load", ctx.attr_string("bogus"));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::LoadOpInvalid);
    }
    // ClearKindInvalid.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        ctx.set_attr(c, "clear_kind", ctx.attr_string("bogus"));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::ClearKindInvalid);
    }
    // BlendInvalid.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        ctx.set_attr(c, "blend", ctx.attr_string("bogus"));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::BlendInvalid);
    }
    // ClearKindFormatMismatch: a uint clear on a FLOAT-format attachment.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32()); // float target
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        ctx.set_attr(c, "clear_kind", ctx.attr_string("uint"));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::ClearKindFormatMismatch);
    }
    // a uint clear on a UINT-format attachment is FINE (the positive companion of the scar).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     b    = mkmain(ctx, *m);
        const TypeId     u32  = ctx.type_int(32U, false);
        Value* const     img  = mkimage(ctx, k, b, u32);
        Operation* const c    = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, u32));
        ctx.set_attr(c, "clear_kind", ctx.attr_string("uint"));
        ctx.set_attr(c, "clear_uint", ctx.attr_int(7));
        Value* atts[1] = {c->result(0U)};
        (void)mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 8, 8);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::None);
    }
    // CompareInvalid (depth).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const d   = mkdepth(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        ctx.set_attr(d, "compare", ctx.attr_string("bogus"));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::CompareInvalid);
    }
    // ScopeOperandNotAttachment: a raw image value fed as a scope operand.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Value*           atts[1] = {img};
        (void)mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 16, 16);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::ScopeOperandNotAttachment);
    }
    // MultipleDepthAttachments.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     b    = mkmain(ctx, *m);
        const TypeId     f32  = ctx.type_f32();
        Value* const     d0i  = mkimage(ctx, k, b, f32);
        Value* const     d1i  = mkimage(ctx, k, b, f32);
        Operation* const d0   = mkdepth(ctx, k, b, d0i, ctx.type_image(ImageDim::Dim2D, f32));
        Operation* const d1   = mkdepth(ctx, k, b, d1i, ctx.type_image(ImageDim::Dim2D, f32));
        Value*           atts[2] = {d0->result(0U), d1->result(0U)};
        (void)mkscope(ctx, k, b, ConstSpan<Value*>(atts, 2U), 16, 16);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::MultipleDepthAttachments);
    }
    // RenderAreaInvalid: width 0.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        Value*           atts[1] = {c->result(0U)};
        (void)mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 0, 480);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::RenderAreaInvalid);
    }
    // SampleCountInvalid: 3 (not a power of two).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        Value*           atts[1] = {c->result(0U)};
        Operation* const sc  = mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 16, 16);
        ctx.set_attr(sc, "sample_count", ctx.attr_int(3));
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::SampleCountInvalid);
    }
    (void)dummy;
}

TEST_CASE("ceir 14b: well-formed render.draw + render.draw_indexed inside a scope pass find_render_misuse", "[ceir][render]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m       = ctx.create_module();
    Block* const            b       = mkmain(ctx, *m);
    const TypeId            f32     = ctx.type_f32();
    Value* const            cimg    = mkimage(ctx, k, b, f32);
    Operation* const        col     = mkcolor(ctx, k, b, cimg, ctx.type_image(ImageDim::Dim2D, f32));
    Value*                  atts[1] = {col->result(0U)};
    Operation* const        sc      = mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 640, 480);
    Block* const            rb      = scope_body(ctx, sc);
    Value*                  none[1] = {nullptr};
    Value* const            vc      = mkindex(ctx, k, rb, 3); // 3 vertices (a fullscreen tri)
    Value* const            ic      = mkindex(ctx, k, rb, 1); // 1 instance
    (void)mkdraw(ctx, k, rb, vc, ic, ConstSpan<Value*>(none, 0U), "prog", "");
    Value* const ib = mkbuffer(ctx, k, rb); // an index buffer (in the scope region for a self-contained module)
    (void)mkdrawi(ctx, k, rb, ic, ic, ib, ConstSpan<Value*>(none, 0U), "prog", "");
    CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::None);
}

TEST_CASE("ceir 14b: a scope with a draw round-trips text == builder (no privileged path)", "[ceir][render]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m       = ctx.create_module();
    Block* const            b       = mkmain(ctx, *m);
    Value* const            cimg    = mkimage(ctx, k, b, ctx.type_f32());
    Operation* const        col     = mkcolor(ctx, k, b, cimg, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
    Value*                  atts[1] = {col->result(0U)};
    Operation* const        sc      = mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 640, 480);
    Block* const            rb      = scope_body(ctx, sc);
    Value*                  none[1] = {nullptr};
    Value* const            vc      = mkindex(ctx, k, rb, 3);
    Value* const            ic      = mkindex(ctx, k, rb, 1);
    (void)mkdraw(ctx, k, rb, vc, ic, ConstSpan<Value*>(none, 0U), "prog", "");

    const String      txt = print(ctx, *m, &root);
    Context           ctx2(&root);
    const Kit         k2(ctx2);
    const ParseResult pr = parse(ctx2, StringView(txt.data(), txt.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String txt2 = print(ctx2, *pr.module, &root);
    CHECK(StringView(txt.data(), txt.size()) == StringView(txt2.data(), txt2.size()));
    CHECK(ctx2.find_render_misuse(*pr.module).kind == RenderMisuseKind::None);
}

TEST_CASE("ceir 14b: find_render_misuse rejects every malformed draw + region misuse", "[ceir][render]")
{
    memory::MallocAllocator root;
    Value*                  none[1] = {nullptr};

    // A scope + its body block over one color attachment (the fixture for the in-scope negatives).
    const auto scope_fixture = [&](Context& ctx, const Kit& k, Module& m, Block*& body) -> void {
        Block* const     b   = mkmain(ctx, m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const col = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        Value*           at[1] = {col->result(0U)};
        Operation* const sc  = mkscope(ctx, k, b, ConstSpan<Value*>(at, 1U), 16, 16);
        body                 = scope_body(ctx, sc);
    };

    // DrawOutsideScope: a draw in the MAIN block (no enclosing scope).
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m  = ctx.create_module();
        Block* const  b  = mkmain(ctx, *m);
        Value* const  vc = mkindex(ctx, k, b, 3);
        Value* const  ic = mkindex(ctx, k, b, 1);
        (void)mkdraw(ctx, k, b, vc, ic, ConstSpan<Value*>(none, 0U), "prog", "");
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::DrawOutsideScope);
    }
    // NestedRenderScope: a scope inside a scope region (0 inner attachments — the nest check fires first).
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        (void)mkscope(ctx, k, rb, ConstSpan<Value*>(none, 0U), 8, 8); // inner scope inside the outer's region
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::NestedRenderScope);
    }
    // ComputeInRenderScope: a compute.dispatch inside a scope region.
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        Value* const g       = mkindex(ctx, k, rb, 1);
        Value*       gops[3] = {g, g, g};
        Operation* const d   = ctx.create_operation(k.disp, ConstSpan<Value*>(gops, 3U), 0U);
        ctx.set_attr(d, "kernel", ctx.attr_symbol("kern"));
        ctx.set_attr(d, "access", ctx.attr_string(""));
        rb->append(d);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::ComputeInRenderScope);
    }
    // ProgramNotSymbol: a draw whose `program` attr is an int.
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        Value* const     vc = mkindex(ctx, k, rb, 3);
        Value* const     ic = mkindex(ctx, k, rb, 1);
        Operation* const dr = mkdraw(ctx, k, rb, vc, ic, ConstSpan<Value*>(none, 0U), "prog", "");
        ctx.set_attr(dr, "program", ctx.attr_int(9)); // overwrite the symbol with an int
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::ProgramNotSymbol);
    }
    // DrawCountNotIndex: a draw whose vertex-count operand is an i32 (not Index).
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        Operation* const badc = ctx.create_operation(k.cst, {}, 1U, ctx.type_i32());
        ctx.set_attr(badc, "value", ctx.attr_int(3));
        rb->append(badc);
        Value* const ic = mkindex(ctx, k, rb, 1);
        (void)mkdraw(ctx, k, rb, badc->result(0U), ic, ConstSpan<Value*>(none, 0U), "prog", "");
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::DrawCountNotIndex);
    }
    // DrawAccessArity: a draw with 0 bindings but a 1-token access string.
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        Value* const vc = mkindex(ctx, k, rb, 3);
        Value* const ic = mkindex(ctx, k, rb, 1);
        (void)mkdraw(ctx, k, rb, vc, ic, ConstSpan<Value*>(none, 0U), "prog", "r"); // 1 token, 0 bindings
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::DrawAccessArity);
    }
    // DrawBindingNotResource: a draw whose one binding is an Index value (not a resource).
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        Value* const vc      = mkindex(ctx, k, rb, 3);
        Value* const ic      = mkindex(ctx, k, rb, 1);
        Value* const notres  = mkindex(ctx, k, rb, 7); // an Index, used as a binding
        Value*       binds[1] = {notres};
        (void)mkdraw(ctx, k, rb, vc, ic, ConstSpan<Value*>(binds, 1U), "prog", "r");
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::DrawBindingNotResource);
    }
    // DrawIndexBufferNotBuffer: a draw_indexed whose index_buffer is an Image (not a Buffer/View-of-buffer).
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        Value* const ic  = mkindex(ctx, k, rb, 1);
        Value* const img = mkimage(ctx, k, rb, ctx.type_f32()); // an image, wrongly used as the index buffer
        (void)mkdrawi(ctx, k, rb, ic, ic, img, ConstSpan<Value*>(none, 0U), "prog", "");
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::DrawIndexBufferNotBuffer);
    }
    // CEIR-16z-2 GeometryModeInvalid: a scene_draw_list whose `geometry` attr is outside {storage, procedural}.
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m  = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        const OpId       sdl = ctx.intern_op("render", "scene_draw_list");
        Operation* const d   = ctx.create_operation(sdl, {}, 0U);
        ctx.set_attr(d, "program", ctx.attr_symbol(StringView("scene")));
        ctx.set_attr(d, "access", ctx.attr_string(StringView("")));
        ctx.set_attr(d, "geometry", ctx.attr_string(StringView("bogus")));
        rb->append(d);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::GeometryModeInvalid);
    }
    // CEIR-16z-2: geometry="procedural" (and absent = storage default) are BOTH accepted.
    {
        Context       ctx(&root);
        const Kit     k(ctx);
        Module* const m  = ctx.create_module();
        Block*        rb = nullptr;
        scope_fixture(ctx, k, *m, rb);
        const OpId       sdl = ctx.intern_op("render", "scene_draw_list");
        Operation* const d   = ctx.create_operation(sdl, {}, 0U);
        ctx.set_attr(d, "program", ctx.attr_symbol(StringView("scene")));
        ctx.set_attr(d, "access", ctx.attr_string(StringView("")));
        ctx.set_attr(d, "geometry", ctx.attr_string(StringView("procedural")));
        rb->append(d);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::None);
    }
}

TEST_CASE("ceir 14c: well-formed indirect + mesh draws inside a scope pass find_render_misuse", "[ceir][render]")
{
    memory::MallocAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m       = ctx.create_module();
    Block* const            b       = mkmain(ctx, *m);
    Value* const            cimg    = mkimage(ctx, k, b, ctx.type_f32());
    Operation* const        col     = mkcolor(ctx, k, b, cimg, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
    Value*                  atts[1] = {col->result(0U)};
    Operation* const        sc      = mkscope(ctx, k, b, ConstSpan<Value*>(atts, 1U), 8, 8);
    Block* const            rb      = scope_body(ctx, sc);
    const auto rop = [&](OpId kind, ConstSpan<Value*> ops, const char* access) {
        Operation* const op = ctx.create_operation(kind, ops, 0U);
        ctx.set_attr(op, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(op, "access", ctx.attr_string(StringView(access)));
        rb->append(op);
        return op;
    };
    Value* const args = mkbuffer(ctx, k, rb);
    Value* const cnt  = mkbuffer(ctx, k, rb);
    Value* const g    = mkindex(ctx, k, rb, 1);
    Value*       ia[1] = {args};
    Operation* const di = rop(k.dind, ConstSpan<Value*>(ia, 1U), "");
    ctx.set_attr(di, "max_draws", ctx.attr_int(16));
    Value* ic[2] = {args, cnt};
    (void)rop(k.dindc, ConstSpan<Value*>(ic, 2U), "");
    Value* mg[3] = {g, g, g};
    (void)rop(k.mesh, ConstSpan<Value*>(mg, 3U), "");
    (void)rop(k.meshind, ConstSpan<Value*>(ia, 1U), "");
    CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::None);
}

TEST_CASE("ceir 14c: find_render_misuse rejects malformed indirect / mesh draws", "[ceir][render]")
{
    memory::MallocAllocator root;
    // A scope + body block over one color attachment; returns the region block.
    const auto scope_rb = [](Context& ctx, const Kit& k, Module& m) -> Block* {
        Block* const     b   = mkmain(ctx, m);
        Value* const     img = mkimage(ctx, k, b, ctx.type_f32());
        Operation* const c   = mkcolor(ctx, k, b, img, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        Value*           at[1] = {c->result(0U)};
        Operation* const sc  = mkscope(ctx, k, b, ConstSpan<Value*>(at, 1U), 8, 8);
        return scope_body(ctx, sc);
    };

    // IndirectArgsNotBuffer: draw_indirect whose %args is an Index value (not a buffer).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m  = ctx.create_module();
        Block* const     rb = scope_rb(ctx, k, *m);
        Value* const     nb = mkindex(ctx, k, rb, 3);
        Value*           ops[1] = {nb};
        Operation* const op = ctx.create_operation(k.dind, ConstSpan<Value*>(ops, 1U), 0U);
        ctx.set_attr(op, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(op, "access", ctx.attr_string(StringView("")));
        rb->append(op);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::IndirectArgsNotBuffer);
    }
    // IndirectCountNotBuffer: draw_indirect_count whose %count is an Index (args is a valid buffer).
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m   = ctx.create_module();
        Block* const     rb  = scope_rb(ctx, k, *m);
        Value* const     args = mkbuffer(ctx, k, rb);
        Value* const     nb   = mkindex(ctx, k, rb, 3);
        Value*           ops[2] = {args, nb};
        Operation* const op  = ctx.create_operation(k.dindc, ConstSpan<Value*>(ops, 2U), 0U);
        ctx.set_attr(op, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(op, "access", ctx.attr_string(StringView("")));
        rb->append(op);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::IndirectCountNotBuffer);
    }
    // MaxDrawsInvalid: draw_indirect with max_draws = 0.
    {
        Context          ctx(&root);
        const Kit        k(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     rb   = scope_rb(ctx, k, *m);
        Value* const     args = mkbuffer(ctx, k, rb);
        Value*           ops[1] = {args};
        Operation* const op   = ctx.create_operation(k.dind, ConstSpan<Value*>(ops, 1U), 0U);
        ctx.set_attr(op, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(op, "access", ctx.attr_string(StringView("")));
        ctx.set_attr(op, "max_draws", ctx.attr_int(0));
        rb->append(op);
        CHECK(ctx.find_render_misuse(*m).kind == RenderMisuseKind::MaxDrawsInvalid);
    }
}
