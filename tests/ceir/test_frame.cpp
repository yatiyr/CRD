// CEIR-15a §39 — the ceir.frame dialect: frame.graph (a REGION op) + frame.pass (the work-unit node) + frame.draw_list
// (an ECS-query value) + frame.history (the prev-frame read of a lifetime=history resource). The frame graph's RESOURCES
// are ordinary resource.declare/import values (NO frame.resource op). Context::find_frame_misuse enforces the contract the
// per-op structural verify cannot: a pass' executor SYMBOL identity, its access tokens {r,w,rw} + arity, its operands
// resource- or draw_list-kinded, the closed-vocab for_each/queue/cull/sort attrs, the frame.history operand's
// lifetime=history-declare identity (the ping-pong contract), and pass-inside-graph. A frame round-trips text == builder
// (§121, no privileged path). ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/frame.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp> // for the ForeignCommandInGraph negative
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

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
    OpId graph, pass, dl, hist, decl, cst, disp;
    explicit Kit(Context& ctx)
        : graph(ctx.intern_op("frame", "graph")), pass(ctx.intern_op("frame", "pass")),
          dl(ctx.intern_op("frame", "draw_list")), hist(ctx.intern_op("frame", "history")),
          decl(ctx.intern_op("resource", "declare")), cst(ctx.intern_op("arith", "const")),
          disp(ctx.intern_op("compute", "dispatch"))
    {
        (void)arith::register_arith_ops(ctx);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)compute::register_compute_ops(ctx); // for the ForeignCommandInGraph negative
        (void)frame::register_dialect(ctx);       // the ops + the draw_list type-class
    }
};
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
// A frame.graph op (resultless, one graph region) + its body block (where the declares/draw-lists/passes live).
Operation* mkgraph(Context& ctx, const Kit& k, Block* b)
{
    Operation* const g = ctx.create_operation(k.graph, {}, 0U, {}, 1U);
    b->append(g);
    return g;
}
Block* graph_body(Context& ctx, Operation* g)
{
    Block* const rb = ctx.create_block(0U);
    g->region(0)->append(rb);
    return rb;
}
// A graph-owned image resource; `lifetime` (empty ⇒ none) sets the resource.declare lifetime attr (history for ping-pong).
Value* mkres(Context& ctx, const Kit& k, Block* rb, TypeId fmt, const char* lifetime)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, fmt));
    if (lifetime[0] != '\0') { ctx.set_attr(d, "lifetime", ctx.attr_string(StringView(lifetime))); }
    rb->append(d);
    return d->result(0U);
}
Value* mkdrawlist(Context& ctx, const Kit& k, Block* rb, const char* cull, const char* sort)
{
    Operation* const op = ctx.create_operation(k.dl, {}, 1U, frame::type_draw_list(ctx));
    if (cull[0] != '\0') { ctx.set_attr(op, "cull", ctx.attr_string(StringView(cull))); }
    if (sort[0] != '\0') { ctx.set_attr(op, "sort", ctx.attr_string(StringView(sort))); }
    rb->append(op);
    return op->result(0U);
}
Value* mkhistory(Context& ctx, const Kit& k, Block* rb, Value* hist_resource, i64 frames_back)
{
    Value* ops[1] = {hist_resource};
    // result type = the operand's underlying resource type (a DISTINCT op result ⇒ its own resource_root).
    Operation* const op = ctx.create_operation(k.hist, ConstSpan<Value*>(ops, 1U), 1U, hist_resource->type());
    if (frames_back != 0) { ctx.set_attr(op, "frames_back", ctx.attr_int(frames_back)); }
    rb->append(op);
    return op->result(0U);
}
Operation* mkpass(Context& ctx, const Kit& k, Block* rb, ConstSpan<Value*> resources, const char* executor,
                  const char* access)
{
    Operation* const op = ctx.create_operation(k.pass, resources, 0U);
    ctx.set_attr(op, "executor", ctx.attr_symbol(StringView(executor)));
    ctx.set_attr(op, "access", ctx.attr_string(StringView(access)));
    rb->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 15a: a well-formed frame graph passes find_frame_misuse", "[ceir][frame]")
{
    memory::GrowableTlsfAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m   = ctx.create_module();
    Block* const            b   = mkmain(ctx, *m);
    const TypeId            f32 = ctx.type_f32();
    Operation* const        g   = mkgraph(ctx, k, b);
    Block* const            rb  = graph_body(ctx, g);
    Value* const            img  = mkres(ctx, k, rb, f32, "");        // a transient target
    Value* const            hist = mkres(ctx, k, rb, f32, "history"); // a ping-pong / TAA history resource
    Value* const            dl   = mkdrawlist(ctx, k, rb, "frustum", "none");
    Value* const            prev = mkhistory(ctx, k, rb, hist, 1); // the prev-frame read
    Value*                  res[3] = {img, prev, dl};
    Operation* const        pass = mkpass(ctx, k, rb, ConstSpan<Value*>(res, 3U), "scene.raster", "w,r,r");
    ctx.set_attr(pass, "queue", ctx.attr_string("graphics"));
    ctx.set_attr(pass, "for_each", ctx.attr_string("none"));
    CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::None);
}

TEST_CASE("ceir 15a: a frame graph round-trips text == builder (no privileged path, sec 121)", "[ceir][frame]")
{
    memory::GrowableTlsfAllocator root;
    Context                 ctx(&root);
    const Kit               k(ctx);
    Module* const           m   = ctx.create_module();
    Block* const            b   = mkmain(ctx, *m);
    const TypeId            f32 = ctx.type_f32();
    Operation* const        g   = mkgraph(ctx, k, b);
    Block* const            rb  = graph_body(ctx, g);
    Value* const            img = mkres(ctx, k, rb, f32, "");
    Value* const            dl  = mkdrawlist(ctx, k, rb, "frustum", "material");
    Value*                  res[2] = {img, dl};
    (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 2U), "scene.raster", "w,r");

    const String txt = print(ctx, *m, &root);
    Context      ctx2(&root);
    const Kit    k2(ctx2); // the parse context needs the frame dialect + its type-class registered
    (void)k2;
    const ParseResult pr = parse(ctx2, StringView(txt.data(), txt.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String txt2 = print(ctx2, *pr.module, &root);
    CHECK(StringView(txt.data(), txt.size()) == StringView(txt2.data(), txt2.size())); // print(parse(print)) fixpoint
    CHECK(ctx2.find_frame_misuse(*pr.module).kind == FrameMisuseKind::None);            // and it re-verifies clean
}

TEST_CASE("ceir 15a: find_frame_misuse rejects every malformed frame", "[ceir][frame]")
{
    const TypeId dummy_fmt = TypeId{}; // set per section
    (void)dummy_fmt;

    SECTION("a frame.pass outside any frame.graph")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m   = ctx.create_module();
        Block* const            b   = mkmain(ctx, *m);
        Value* const            img = mkres(ctx, k, b, ctx.type_f32(), ""); // declared in the func body, no graph
        Value*                  res[1] = {img};
        (void)mkpass(ctx, k, b, ConstSpan<Value*>(res, 1U), "scene.raster", "w");
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::PassOutsideGraph);
    }
    SECTION("a frame.pass executor that is not a Symbol")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            img = mkres(ctx, k, rb, ctx.type_f32(), "");
        Value*                  res[1] = {img};
        Operation* const        p = ctx.create_operation(k.pass, ConstSpan<Value*>(res, 1U), 0U);
        ctx.set_attr(p, "executor", ctx.attr_string("scene.raster")); // a STRING, not a Symbol
        ctx.set_attr(p, "access", ctx.attr_string("w"));
        rb->append(p);
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::ExecutorNotSymbol);
    }
    SECTION("a frame.pass access token count != operand count")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            a  = mkres(ctx, k, rb, ctx.type_f32(), "");
        Value* const            c  = mkres(ctx, k, rb, ctx.type_f32(), "");
        Value*                  res[2] = {a, c};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 2U), "scene.raster", "w"); // 1 token, 2 operands
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::PassAccessArity);
    }
    SECTION("a frame.pass access token outside {r,w,rw}")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            a  = mkres(ctx, k, rb, ctx.type_f32(), "");
        Value*                  res[1] = {a};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "x");
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::PassAccessInvalid);
    }
    SECTION("a frame.pass operand that is not resource- nor draw_list-kinded")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Operation* const        idx = ctx.create_operation(k.cst, {}, 1U, ctx.type_index()); // an Index value, not a resource
        rb->append(idx);
        Value*                  res[1] = {idx->result(0U)};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "r");
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::PassOperandNotResource);
    }
    SECTION("a frame.pass queue outside {graphics, async}")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            a  = mkres(ctx, k, rb, ctx.type_f32(), "");
        Value*                  res[1] = {a};
        Operation* const        p = mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "w");
        ctx.set_attr(p, "queue", ctx.attr_string("turbo"));
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::QueueInvalid);
    }
    SECTION("a frame.draw_list cull outside its vocabulary")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        (void)mkdrawlist(ctx, k, rb, "sometimes", "none");
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::DrawListCullInvalid);
    }
    SECTION("a frame.draw_list limit < 0")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Operation* const        op = ctx.create_operation(k.dl, {}, 1U, frame::type_draw_list(ctx));
        ctx.set_attr(op, "limit", ctx.attr_int(-1));
        rb->append(op);
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::DrawListLimitInvalid);
    }
    SECTION("a frame.history over a NON-history resource")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            transient = mkres(ctx, k, rb, ctx.type_f32(), ""); // NOT lifetime=history
        (void)mkhistory(ctx, k, rb, transient, 1);
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::HistoryOperandNotHistory);
    }
    SECTION("a frame.history frames_back < 1")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            hist = mkres(ctx, k, rb, ctx.type_f32(), "history");
        (void)mkhistory(ctx, k, rb, hist, 0);      // frames_back attr present + 0 -> invalid (0 means absent in mkhistory)
        // set it explicitly to 0 to exercise the check (mkhistory skips setting when 0), so build it raw:
        Value* ops[1] = {hist};
        Operation* const h = ctx.create_operation(k.hist, ConstSpan<Value*>(ops, 1U), 1U, hist->type());
        ctx.set_attr(h, "frames_back", ctx.attr_int(0));
        rb->append(h);
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::HistoryFramesBackInvalid);
    }
    SECTION("a foreign GPUCommand op directly in a frame.graph region")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        // a compute.dispatch (a GPUCommand) directly in the graph, not wrapped in a frame.pass.
        Operation* const        gx = ctx.create_operation(k.cst, {}, 1U, ctx.type_index());
        rb->append(gx);
        Value* grid[3] = {gx->result(0U), gx->result(0U), gx->result(0U)};
        Operation* const d = ctx.create_operation(k.disp, ConstSpan<Value*>(grid, 3U), 0U);
        ctx.set_attr(d, "kernel", ctx.attr_symbol("k"));
        ctx.set_attr(d, "access", ctx.attr_string(""));
        rb->append(d);
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::ForeignCommandInGraph);
    }
    // ── CEIR-15c-1a: the NEW-IN-CEIR structural guards (ceir.frame is a strict superset of FrameGraphDesc). ──
    SECTION("a frame.pass operand defined OUTSIDE the frame.graph region")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Value* const            outside = mkres(ctx, k, b, ctx.type_f32(), ""); // declared in the FUNC BODY, not the graph
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value*                  res[1] = {outside};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "w"); // resource-typed, so not PassOperandNotResource
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::OperandOutsideGraph);
    }
    SECTION("more than one frame.graph in the module")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g1 = mkgraph(ctx, k, b);
        (void)graph_body(ctx, g1);
        Operation* const        g2 = mkgraph(ctx, k, b); // a second graph has no FrameGraphDesc home
        (void)graph_body(ctx, g2);
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::MultipleGraphs);
    }
    SECTION("a frame.pass READS a lifetime=history resource directly (not through frame.history)")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            hist = mkres(ctx, k, rb, ctx.type_f32(), "history");
        Value*                  res[1] = {hist};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "r"); // a DIRECT read of the declare (Fork-B trap)
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::HistoryReadNotThroughFrameHistory);
    }
    SECTION("a frame.pass WRITES a frame.history result (writing the previous frame)")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            hist = mkres(ctx, k, rb, ctx.type_f32(), "history");
        Value* const            prev = mkhistory(ctx, k, rb, hist, 1);
        Value*                  res[1] = {prev};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "w"); // writing a PREVIOUS-frame value is nonsense
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::HistoryWriteThroughHistory);
    }
    // ── CEIR-15c-1c-2 §8/§9: the OTHER frame ops outside a graph + a draw-list write. ──
    SECTION("a frame.draw_list OUTSIDE any frame.graph")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m = ctx.create_module();
        Block* const            b = mkmain(ctx, *m);
        (void)mkdrawlist(ctx, k, b, "frustum", "none"); // in the func body, not a graph
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::DrawListOutsideGraph);
    }
    SECTION("a frame.history OUTSIDE any frame.graph")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m    = ctx.create_module();
        Block* const            b    = mkmain(ctx, *m);
        Value* const            hist = mkres(ctx, k, b, ctx.type_f32(), "history"); // a history declare in the func body
        (void)mkhistory(ctx, k, b, hist, 1);                                        // frame.history over it, in the func body
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::HistoryOutsideGraph);
    }
    SECTION("a frame.pass carries a WRITE token on a draw-list operand")
    {
        memory::GrowableTlsfAllocator root;
        Context                 ctx(&root);
        const Kit               k(ctx);
        Module* const           m  = ctx.create_module();
        Block* const            b  = mkmain(ctx, *m);
        Operation* const        g  = mkgraph(ctx, k, b);
        Block* const            rb = graph_body(ctx, g);
        Value* const            dl = mkdrawlist(ctx, k, rb, "frustum", "none");
        Value*                  res[1] = {dl};
        (void)mkpass(ctx, k, rb, ConstSpan<Value*>(res, 1U), "scene.raster", "w"); // ⛔ a draw list is queried, never written
        CHECK(ctx.find_frame_misuse(*m).kind == FrameMisuseKind::DrawListNotWritable);
    }
}
