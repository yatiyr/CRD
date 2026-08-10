// CEIR-13z-1 — the crd-ceir-gpu EXECUTION seam (ADR-0126), the DEVICE-FREE half. `validate_lowered` + `execute_lowered` bind
// a 13d-lowered command list to the ADR-0100 IComputeContext surface (a resolver → ComputePipeline; a Value*→ComputeBuffer*
// table). ⛔ These are the ALWAYS-RUNS tests (no GPU): a FAKE resolver (returns the `user` pointer), FAKE buffers, and a FAKE
// recorder that COUNTS dispatch calls prove the typed ExecuteError paths + the recording path with no device — the guard
// against a device suite that skips every case reading as PASS. The real add/reduce/scan/FFT device execution is 13z-1b+.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/transfer_ops.hpp>
#include <crd/ceir/render.hpp> // CEIR-14b: the render.scope region-recursion lowering + the hazard-hole
#include <crd/ceir/exec.hpp> // CEIR-13z-4 leg 1b: the core Interpreter REFUSES a dispatch (typed NoSemantics)
#include <crd/ceir/func.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace crd::ceir;      // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;

namespace
{
// ── device-free fakes ── ComputePipeline/ComputeRecorder are NOT abstract-with-derefs on these paths; a trivial subclass
// suffices. The resolver returns its `user` (a FakePipe* or nullptr); validation never dereferences the pipeline.
struct FakePipe : crd::gpu::ComputePipeline
{
};
struct FakeBuf : crd::gpu::ComputeBuffer
{
    void* map() noexcept override { return nullptr; }
    void  unmap() noexcept override {}
};
struct FakeRec : crd::gpu::ComputeRecorder
{
    int                           dispatches = 0;
    int                           barriers   = 0;
    crd::u32                      last_bufs  = 0;
    crd::u32                      last_gx    = 0;
    const crd::gpu::ComputeBuffer* last_barrier_buf = nullptr; // ⭐ 13z-3 part 2: which buffer the last barrier hit
    crd::gpu::ComputeAccess       last_from  = crd::gpu::ComputeAccess::ShaderRead;
    crd::gpu::ComputeAccess       last_to    = crd::gpu::ComputeAccess::ShaderRead;
    void copy(crd::gpu::ComputeBuffer&, crd::gpu::ComputeBuffer&, crd::u64, crd::u64, crd::u64) override {}
    void barrier(crd::gpu::ComputeBuffer& buf, crd::gpu::ComputeAccess from, crd::gpu::ComputeAccess to) override
    {
        ++barriers;
        last_barrier_buf = &buf;
        last_from        = from;
        last_to          = to;
    }
    void dispatch(crd::gpu::ComputePipeline&, ConstSpan<crd::gpu::ComputeBuffer*> binds, const void*, crd::u32, crd::u32 gx,
                  crd::u32 gy, crd::u32 gz) override
    {
        ++dispatches;
        last_bufs = static_cast<crd::u32>(binds.size());
        last_gx   = gx;
        (void)gy;
        (void)gz;
    }
};
crd::gpu::ComputePipeline* resolve_from_user(const Operation*, void* user)
{
    return static_cast<crd::gpu::ComputePipeline*>(user);
}

struct Kit
{
    OpId cst, decl, view, disp, upload, col, scope, draw, dind, mesh;
    explicit Kit(Context& c)
        : cst(c.intern_op("arith", "const")), decl(c.intern_op("resource", "declare")),
          view(c.intern_op("resource", "view")), disp(c.intern_op("compute", "dispatch")),
          upload(c.intern_op("transfer", "upload")), col(c.intern_op("render", "color_attachment")),
          scope(c.intern_op("render", "scope")), draw(c.intern_op("render", "draw")),
          dind(c.intern_op("render", "draw_indirect")), mesh(c.intern_op("render", "mesh_dispatch"))
    {
        (void)arith::register_arith_ops(c);
        (void)func::register_dialect(c);
        (void)resource::register_resource_ops(c);
        (void)compute::register_compute_ops(c);
        (void)transfer::register_transfer_ops(c);
        (void)render::register_dialect(c); // CEIR-14b: the render ops + attachment type-classes
    }
};
// CEIR-14b render builders.
Value* declimg(Context& c, const Kit& k, Block* b)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_image(ImageDim::Dim2D, c.type_f32()));
    b->append(d);
    return d->result(0U);
}
Value* coloratt(Context& c, const Kit& k, Block* b, Value* img)
{
    Value*           ops[1] = {img};
    Operation* const op     = c.create_operation(k.col, ConstSpan<Value*>(ops, 1U), 1U, render::type_color_attachment(c, img->type()));
    b->append(op);
    return op->result(0U);
}
Operation* scope_op(Context& c, const Kit& k, Block* b, Value* const* atts, crd::u32 na, i64 w, i64 h)
{
    Operation* const op = c.create_operation(k.scope, ConstSpan<Value*>(atts, na), 0U, {}, 1U);
    c.set_attr(op, "width", c.attr_int(w));
    c.set_attr(op, "height", c.attr_int(h));
    b->append(op);
    return op;
}
Block* scope_body(Context& c, Operation* sc)
{
    Block* const rb = c.create_block(0U);
    sc->region(0)->append(rb);
    return rb;
}
Operation* draw_op(Context& c, const Kit& k, Block* rb, Value* vc, Value* ic, Value* const* binds, crd::u32 nb,
                   const char* access, const char* prog)
{
    Value* ops[16];
    ops[0] = vc;
    ops[1] = ic;
    for (crd::u32 i = 0; i < nb; ++i) { ops[2 + i] = binds[i]; }
    Operation* const op = c.create_operation(k.draw, ConstSpan<Value*>(ops, 2U + nb), 0U);
    c.set_attr(op, "program", c.attr_symbol(StringView(prog)));
    c.set_attr(op, "access", c.attr_string(StringView(access)));
    rb->append(op);
    return op;
}
Value* konst(Context& c, const Kit& k, Block* b, i64 v)
{
    Operation* const o = c.create_operation(k.cst, {}, 1U, c.type_index());
    c.set_attr(o, "value", c.attr_int(v));
    b->append(o);
    return o->result(0U);
}
Value* declbuf(Context& c, const Kit& k, Block* b)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_buffer(BufferMode::Plain, c.type_f32()));
    b->append(d);
    return d->result(0U);
}
// CEIR-14d: a graph-owned RESOURCE TABLE (a §157 bindless table of a buffer element) — a resource.declare of the CEIR-3c
// ResourceTable type. It binds like any other resource (ceir_is_resource_kind covers it).
Value* decltable(Context& c, const Kit& k, Block* b)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_resource_table(c.type_buffer(BufferMode::Plain, c.type_f32())));
    b->append(d);
    return d->result(0U);
}
Value* view_of(Context& c, const Kit& k, Block* b, Value* res)
{
    Value*           vo[3] = {res, konst(c, k, b, 0), konst(c, k, b, 16)};
    const TypeId     vty = c.type_view(c.type_buffer(BufferMode::Plain, c.type_f32()), static_cast<crd::u32>(ViewRange::Byte));
    Operation* const v   = c.create_operation(k.view, ConstSpan<Value*>(vo, 3U), 1U, vty);
    b->append(v);
    return v->result(0U);
}
// A direct dispatch over (grid,grid,grid) binding `binds[0..nbind)` with a comma `access` of exactly nbind tokens.
Operation* dispatch_bufs(Context& c, const Kit& k, Block* b, Value* grid, Value* const* binds, crd::u32 nbind,
                         const char* access, const char* kernel)
{
    Value * ops[64];
    ops[0] = grid;
    ops[1] = grid;
    ops[2] = grid;
    for (crd::u32 i = 0; i < nbind; ++i) { ops[3 + i] = binds[i]; }
    Operation* const d = c.create_operation(k.disp, ConstSpan<Value*>(ops, 3U + nbind), 0U);
    c.set_attr(d, "kernel", c.attr_symbol(StringView(kernel)));
    c.set_attr(d, "access", c.attr_string(StringView(access)));
    b->append(d);
    return d;
}
} // namespace

TEST_CASE("ceir 13z: execute seam validates a good dispatch and records it (device-free, fake recorder)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    FakePipe                     pipe;
    FakeBuf                      ba;
    FakeBuf                      bb;

    Block* const b     = ctx.create_block(0U);
    Value* const g     = konst(ctx, k, b, 1);
    Value* const va    = declbuf(ctx, k, b);
    Value* const vb    = declbuf(ctx, k, b);
    Value*       bd[2] = {va, vb};
    (void)dispatch_bufs(ctx, k, b, g, bd, 2U, "rw,r", "add");

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() == 1U);

    ResolvedBinding       binds[2] = {{va, &ba}, {vb, &bb}};
    const ConstSpan<ResolvedBinding> table(binds, 2U);
    const ConstSpan<LoweredCommand>  clist(cmds.data(), cmds.size());

    CHECK(validate_lowered(ctx, clist, resolve_from_user, &pipe, table) == ExecuteError::None);

    FakeRec rec;
    CHECK(execute_lowered(ctx, clist, rec, resolve_from_user, &pipe, table) == ExecuteError::None);
    CHECK(rec.dispatches == 1);
    CHECK(rec.last_bufs == 2U); // both bindings gathered in operand order
    CHECK(rec.last_gx == 1U);
}

TEST_CASE("ceir 13z: a resource.view binding normalizes to its root before the table lookup (13d part-3)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    FakePipe                     pipe;
    FakeBuf                      ba;

    Block* const b    = ctx.create_block(0U);
    Value* const g    = konst(ctx, k, b, 1);
    Value* const va   = declbuf(ctx, k, b);
    Value* const view = view_of(ctx, k, b, va); // bind a VIEW of the buffer
    Value*       bd[1] = {view};
    (void)dispatch_bufs(ctx, k, b, g, bd, 1U, "r", "k");

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    // the table maps the ROOT buffer, not the view -> resource_root normalization must find it.
    ResolvedBinding       binds[1] = {{va, &ba}};
    CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, &pipe,
                           ConstSpan<ResolvedBinding>(binds, 1U))
          == ExecuteError::None);
}

TEST_CASE("ceir 13z: the typed ExecuteError paths (device-free always-runs)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    FakePipe                     pipe;
    FakeBuf                      ba;

    // UnresolvedKernel: the resolver returns nullptr (user == nullptr).
    {
        Block* const b     = ctx.create_block(0U);
        Value* const g     = konst(ctx, k, b, 1);
        Value* const va    = declbuf(ctx, k, b);
        Value*       bd[1] = {va};
        (void)dispatch_bufs(ctx, k, b, g, bd, 1U, "r", "k");
        Array<LoweredCommand> cmds(&root);
        lower_region(ctx, *b, cmds);
        ResolvedBinding binds[1] = {{va, &ba}};
        CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, nullptr,
                               ConstSpan<ResolvedBinding>(binds, 1U))
              == ExecuteError::UnresolvedKernel);
    }
    // ZeroDispatch: a resolved zero grid group.
    {
        Block* const b     = ctx.create_block(0U);
        Value* const g0    = konst(ctx, k, b, 0);
        Value* const va    = declbuf(ctx, k, b);
        Value*       bd[1] = {va};
        (void)dispatch_bufs(ctx, k, b, g0, bd, 1U, "r", "k");
        Array<LoweredCommand> cmds(&root);
        lower_region(ctx, *b, cmds);
        ResolvedBinding binds[1] = {{va, &ba}};
        CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, &pipe,
                               ConstSpan<ResolvedBinding>(binds, 1U))
              == ExecuteError::ZeroDispatch);
    }
    // UnmappedBinding: a binding whose root has no table entry (empty table).
    {
        Block* const b     = ctx.create_block(0U);
        Value* const g     = konst(ctx, k, b, 1);
        Value* const va    = declbuf(ctx, k, b);
        Value*       bd[1] = {va};
        (void)dispatch_bufs(ctx, k, b, g, bd, 1U, "r", "k");
        Array<LoweredCommand> cmds(&root);
        lower_region(ctx, *b, cmds);
        CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, &pipe,
                               ConstSpan<ResolvedBinding>(static_cast<const ResolvedBinding*>(nullptr), 0U))
              == ExecuteError::UnmappedBinding);
    }
    // BindingArity: more than kMaxBindings (16) binding operands.
    {
        Block* const b   = ctx.create_block(0U);
        Value* const g   = konst(ctx, k, b, 1);
        Value*       bd[17];
        char         acc[64];
        for (crd::u32 i = 0; i < 17U; ++i)
        {
            bd[i]        = declbuf(ctx, k, b);
            acc[2U * i]  = 'r';
            acc[2U * i + 1U] = (i + 1U < 17U) ? ',' : '\0';
        }
        (void)dispatch_bufs(ctx, k, b, g, bd, 17U, acc, "k");
        Array<LoweredCommand> cmds(&root);
        lower_region(ctx, *b, cmds);
        CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, &pipe,
                               ConstSpan<ResolvedBinding>(static_cast<const ResolvedBinding*>(nullptr), 0U))
              == ExecuteError::BindingArity);
    }
    // UnsupportedCommand: a transfer command (dispatch-only at 13z-1).
    {
        Block* const     b   = ctx.create_block(0U);
        Value* const     va  = declbuf(ctx, k, b);
        Value*           uo[1] = {va};
        Operation* const up  = ctx.create_operation(k.upload, ConstSpan<Value*>(uo, 1U), 0U);
        b->append(up);
        Array<LoweredCommand> cmds(&root);
        lower_region(ctx, *b, cmds);
        CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, &pipe,
                               ConstSpan<ResolvedBinding>(static_cast<const ResolvedBinding*>(nullptr), 0U))
              == ExecuteError::UnsupportedCommand);
    }
    // UnsupportedCommand: a dynamic (non-const) grid dispatch (const-grid only at 13z-1).
    {
        Block* const b     = ctx.create_block(0U);
        Value* const dyn   = declbuf(ctx, k, b); // a non-const value as the grid -> dynamic_grid
        Value* const va    = declbuf(ctx, k, b);
        Value*       bd[1] = {va};
        (void)dispatch_bufs(ctx, k, b, dyn, bd, 1U, "r", "k");
        Array<LoweredCommand> cmds(&root);
        lower_region(ctx, *b, cmds);
        ResolvedBinding binds[1] = {{va, &ba}};
        CHECK(validate_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), resolve_from_user, &pipe,
                               ConstSpan<ResolvedBinding>(binds, 1U))
              == ExecuteError::UnsupportedCommand);
    }
}

TEST_CASE("ceir 13z: execute_lowered refuses a bad program and records NOTHING", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    FakePipe                     pipe;

    Block* const     b   = ctx.create_block(0U);
    Value* const     va  = declbuf(ctx, k, b);
    Value*           uo[1] = {va};
    Operation* const up  = ctx.create_operation(k.upload, ConstSpan<Value*>(uo, 1U), 0U);
    b->append(up);
    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);

    FakeRec rec;
    CHECK(execute_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), rec, resolve_from_user, &pipe,
                          ConstSpan<ResolvedBinding>(static_cast<const ResolvedBinding*>(nullptr), 0U))
          == ExecuteError::UnsupportedCommand);
    CHECK(rec.dispatches == 0); // refused BEFORE recording
}

// CEIR-13z-2 PROBE (fork D): does a compute.dispatch asset whose `kernel` attr is a DANGLING @symbol (defined NOWHERE in
// the module) round-trip through the CEIR text form? parse.hpp re-registers symbol-DEFINING ops; a pure symbol-REFERENCE
// attr with no definition is the unverified case. If this REQUIRE fails, 13z-2's text-authoring dimension is reshaped.
TEST_CASE("ceir 13z-2 probe: a compute.dispatch module with a dangling @kernel symbol round-trips through text", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    Module* const m = ctx.create_module();
    Block* const  b = ctx.create_block(0U);
    m->body()->append(b);
    Value* const g  = konst(ctx, k, b, 1);
    Value* const va = declbuf(ctx, k, b);
    Value* const vb = declbuf(ctx, k, b);
    Value*       ops[5] = {g, g, g, va, vb};
    Operation* const d  = ctx.create_operation(k.disp, ConstSpan<Value*>(ops, 5U), 0U);
    ctx.set_attr(d, "kernel", ctx.attr_symbol(StringView("add")));
    ctx.set_attr(d, "access", ctx.attr_string(StringView("r,w")));
    b->append(d);

    const crd::containers::String text1 = print(ctx, *m, &root);
    CHECK(std::strstr(text1.c_str(), "@add") != nullptr); // the dangling kernel symbol printed

    Context           ctx2(&root);
    const ParseResult pr = parse(ctx2, text1);
    REQUIRE(pr.ok); // ⭐ THE PROBE — the dangling symbol reference parses (or reshapes 13z-2)
    REQUIRE(pr.module != nullptr);
    const crd::containers::String text2 = print(ctx2, *pr.module, &root);
    CHECK(std::strcmp(text1.c_str(), text2.c_str()) == 0); // byte-exact round-trip

    // and the PARSED module lowers to the same one-dispatch command list as the builder module.
    Block* const           pb = pr.module->body()->first_block();
    REQUIRE(pb != nullptr);
    Array<LoweredCommand>  cmds(&root);
    lower_region(ctx2, *pb, cmds);
    REQUIRE(cmds.size() == 1U);
    CHECK(cmds[0].kind == LoweredKind::Dispatch);
}

// CEIR-13z-2 (§121 text≡builder): a `reduce` compute asset authored as an EMBEDDED TEXT literal parses to the SAME canonical
// module as the builder path, and lowers to the same command list. The device-execution leg (the parsed module runs on a
// real GPU) lives in the Vulkan target (kir-linked). ⭐ assert is print(parse(text)) == print(builder) — the literal need
// only PARSE; the canonical print normalizes both authoring paths.
TEST_CASE("ceir 13z-2: a text-authored reduce asset == the builder-authored asset (canonical + lowering)", "[ceir][ceir-gpu]")
{
    const char* const k_reduce_text = "module {\n"
                                      "  ^bb0:\n"
                                      "    %0 = arith.const() {value = 1} : !index\n"
                                      "    %1 = resource.declare() : !buffer<plain,!f32>\n"
                                      "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                      "    compute.dispatch(%0, %0, %0, %1, %2) {access = \"r,w\", kernel = @reduce}\n"
                                      "}\n";
    crd::memory::MallocAllocator root;

    // BUILDER path: the same reduce asset via the API (2 buffers, access r,w, kernel @reduce).
    Context       ctxb(&root);
    const Kit     kb(ctxb);
    Module* const mb = ctxb.create_module();
    Block* const  bb = ctxb.create_block(0U);
    mb->body()->append(bb);
    Value* const     g   = konst(ctxb, kb, bb, 1);
    Value* const     v1  = declbuf(ctxb, kb, bb);
    Value* const     v2  = declbuf(ctxb, kb, bb);
    Value*           ops[5] = {g, g, g, v1, v2};
    Operation* const d   = ctxb.create_operation(kb.disp, ConstSpan<Value*>(ops, 5U), 0U);
    ctxb.set_attr(d, "kernel", ctxb.attr_symbol(StringView("reduce")));
    ctxb.set_attr(d, "access", ctxb.attr_string(StringView("r,w")));
    bb->append(d);
    const crd::containers::String text_builder = print(ctxb, *mb, &root);

    // TEXT path: parse the authored literal.
    Context           ctxt(&root);
    const ParseResult pr = parse(ctxt, StringView(k_reduce_text));
    REQUIRE(pr.ok); // the hand-authored text is valid CEIR
    const crd::containers::String text_parsed = print(ctxt, *pr.module, &root);

    // §121 assert 1: the two authoring paths normalize to the SAME canonical text (byte-exact).
    CHECK(std::strcmp(text_parsed.c_str(), text_builder.c_str()) == 0);

    // §121 assert 2: both lower to an equivalent one-dispatch command list.
    Array<LoweredCommand> cb(&root);
    Array<LoweredCommand> ct(&root);
    lower_region(ctxb, *bb, cb);
    lower_region(ctxt, *pr.module->body()->first_block(), ct);
    REQUIRE(cb.size() == 1U);
    REQUIRE(ct.size() == cb.size());
    CHECK(ct[0].kind == cb[0].kind);
    CHECK(ct[0].groups_x == cb[0].groups_x);
}

// CEIR-13z-3 part 2: execute_lowered REPLAYS a Barrier as rec.barrier(root_buffer, from, to) — proven device-free via the
// FakeRec (records barrier calls). A write-then-read on ONE buffer -> exactly one RAW barrier on that buffer.
TEST_CASE("ceir 13z-3: execute_lowered replays a barrier as rec.barrier on the shared root (device-free)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    FakePipe                     pipe;
    FakeBuf                      fb;

    Block* const b   = ctx.create_block(0U);
    Value* const g   = konst(ctx, k, b, 1);
    Value* const buf = declbuf(ctx, k, b);
    Value*       bd[1] = {buf};
    (void)dispatch_bufs(ctx, k, b, g, bd, 1U, "w", "a"); // writes buf
    (void)dispatch_bufs(ctx, k, b, g, bd, 1U, "r", "b"); // reads buf -> RAW

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() == 3U);
    CHECK(cmds[1].kind == LoweredKind::Barrier);
    CHECK(cmds[1].hazard == HazardKind::Raw);
    CHECK(cmds[1].resource == buf);

    ResolvedBinding binds[1] = {{buf, &fb}};
    FakeRec         rec;
    CHECK(execute_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), rec, resolve_from_user, &pipe,
                          ConstSpan<ResolvedBinding>(binds, 1U))
          == ExecuteError::None);
    CHECK(rec.dispatches == 2);
    CHECK(rec.barriers == 1);
    CHECK(rec.last_barrier_buf == &fb);                              // the barrier hit the shared buffer
    CHECK(rec.last_from == crd::gpu::ComputeAccess::ShaderWrite);    // RAW: write -> read
    CHECK(rec.last_to == crd::gpu::ComputeAccess::ShaderRead);
}

TEST_CASE("ceir 13z-3: execute_lowered emits PER-RESOURCE barriers (2 buffers from 2 writers -> 2 rec.barrier calls)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    FakePipe                     pipe;
    FakeBuf                      fa;
    FakeBuf                      fb;

    Block* const b  = ctx.create_block(0U);
    Value* const g  = konst(ctx, k, b, 1);
    Value* const va = declbuf(ctx, k, b);
    Value* const vb = declbuf(ctx, k, b);
    Value*       aw[1] = {va};
    Value*       bw[1] = {vb};
    (void)dispatch_bufs(ctx, k, b, g, aw, 1U, "w", "a"); // writes bufA
    (void)dispatch_bufs(ctx, k, b, g, bw, 1U, "w", "b"); // writes bufB
    Value* cr[2] = {va, vb};
    (void)dispatch_bufs(ctx, k, b, g, cr, 2U, "r,r", "c"); // reads BOTH -> 2 barriers

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() == 5U); // D, D, Barrier(bufA), Barrier(bufB), D

    ResolvedBinding binds[2] = {{va, &fa}, {vb, &fb}};
    FakeRec         rec;
    CHECK(execute_lowered(ctx, ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), rec, resolve_from_user, &pipe,
                          ConstSpan<ResolvedBinding>(binds, 2U))
          == ExecuteError::None);
    CHECK(rec.dispatches == 3);
    CHECK(rec.barriers == 2); // ⭐ one per conflicting root resource (the completion: a single barrier would drop one)
}

// CEIR-13z-4 leg 1b (fork G): the CORE Interpreter (the §118 reference) REFUSES a compute.dispatch with a TYPED NoSemantics
// error — dispatch has no installed core semantics (I5: the core never learns kernels; the §150 forward). ⛔ this is a
// graceful typed refusal, NOT a crash and NOT a fake skip — installing skip/no-op dispatch semantics in core would silently
// pre-empt §150. The buffer-level reference is the bridge/test side (execute_lowered_cpu, leg 1a).
TEST_CASE("ceir 13z-4: the core Interpreter refuses a compute.dispatch with typed NoSemantics (I5 stays device-blind)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    Module* const    m     = ctx.create_module();
    const TypeId     bufty = ctx.type_buffer(BufferMode::Plain, ctx.type_f32());
    Operation* const f     = func::create_func(ctx, *m, StringView("main"), Visibility::Public, 1U, bufty);
    Block* const     b     = func::func_body_block(f);
    REQUIRE(b != nullptr);
    // a resultless compute.dispatch binding the func's buffer ARGUMENT (no resource.declare, which would also be NoSemantics).
    Value* const     g     = konst(ctx, k, b, 1);
    Value*           ops[4] = {g, g, g, b->arg(0U)};
    Operation* const d     = ctx.create_operation(k.disp, ConstSpan<Value*>(ops, 4U), 0U);
    ctx.set_attr(d, "kernel", ctx.attr_symbol(StringView("kern")));
    ctx.set_attr(d, "access", ctx.attr_string(StringView("r")));
    b->append(d);
    b->append(func::create_return(ctx, {}));

    exec::Interpreter in(ctx);
    exec::install_builtin_semantics(in); // arith/core/func ONLY — NOT compute/resource
    i64                    a0[1] = {0};
    const exec::ExecResult r     = in.invoke(*m, StringView("main"), ConstSpan<i64>(a0, 1U));
    CHECK_FALSE(r.ok());
    CHECK(r.error == exec::ExecError::NoSemantics); // ⭐ the typed refusal
    CHECK(r.op == d);                               // pointing at the dispatch
}

// ── CEIR-14b part (ii): the render.scope region-recursion lowering (device-free) ──

TEST_CASE("ceir 14b: a render.scope with two draws lowers to BeginRender, Draw, Draw, EndRender", "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 img = declimg(ctx, k, b);
    Value* const                 col = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    Operation* const             sc  = scope_op(ctx, k, b, atts, 1U, 640, 480);
    Block* const                 rb  = scope_body(ctx, sc);
    Value* const                 vc  = konst(ctx, k, rb, 3); // 3 vertices
    Value* const                 ic  = konst(ctx, k, rb, 1); // 1 instance
    (void)draw_op(ctx, k, rb, vc, ic, nullptr, 0U, "", "prog");
    (void)draw_op(ctx, k, rb, vc, ic, nullptr, 0U, "", "prog");

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() == 4U); // the color_attachment + the konsts are Pure — they emit nothing
    CHECK(cmds[0].kind == LoweredKind::BeginRender);
    CHECK(cmds[1].kind == LoweredKind::Draw);
    CHECK(cmds[2].kind == LoweredKind::Draw);
    CHECK(cmds[3].kind == LoweredKind::EndRender);
    CHECK(cmds[0].op == sc); // BeginRender + EndRender carry the scope op (the 14z executor materializes the RenderingDesc)
    CHECK(cmds[3].op == sc);
}

TEST_CASE("ceir 14b: a dispatch writing B then a scope-with-a-draw-binding-B emits a barrier before the scope (hazard hole)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 g   = konst(ctx, k, b, 1);
    Value* const                 buf = declbuf(ctx, k, b); // buffer B
    Value*                       wb[1] = {buf};
    (void)dispatch_bufs(ctx, k, b, g, wb, 1U, "w", "writer"); // the dispatch WRITES B
    Value* const                 img = declimg(ctx, k, b);
    Value* const                 col = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    Operation* const             sc  = scope_op(ctx, k, b, atts, 1U, 8, 8);
    Block* const                 rb  = scope_body(ctx, sc);
    Value* const                 vc  = konst(ctx, k, rb, 3);
    Value* const                 ic  = konst(ctx, k, rb, 1);
    Value*                       db[1] = {buf};                          // ⭐ the DRAW (inside the region) BINDS B
    (void)draw_op(ctx, k, rb, vc, ic, db, 1U, "r", "prog");

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    // ⭐ THE LOAD-BEARING ASSERTION: the scope's conservative ambient MemoryReadWrite hazards against the dispatch's write of
    // B — even though the flat walk at the scope sees only its attachment operand, not the draw's binding inside the region.
    int begin = -1;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(cmds.size()); ++i)
    {
        if (cmds[i].kind == LoweredKind::BeginRender) { begin = static_cast<int>(i); break; }
    }
    REQUIRE(begin > 0);
    CHECK(cmds[static_cast<crd::u32>(begin - 1)].kind == LoweredKind::Barrier); // a barrier lands BEFORE the scope
    CHECK(cmds[static_cast<crd::u32>(begin - 1)].after == sc);                  // ...ordered into the scope
    CHECK(cmds[static_cast<crd::u32>(begin - 1)].before != nullptr);            // ...from the earlier writer (the dispatch)
}

TEST_CASE("ceir 14b: validate_lowered + execute_lowered reject render kinds typed (UnsupportedCommand, the Transfer mirror)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 img = declimg(ctx, k, b);
    Value* const                 col = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    Operation* const             sc  = scope_op(ctx, k, b, atts, 1U, 8, 8);
    Block* const                 rb  = scope_body(ctx, sc);
    Value* const                 vc  = konst(ctx, k, rb, 3);
    Value* const                 ic  = konst(ctx, k, rb, 1);
    (void)draw_op(ctx, k, rb, vc, ic, nullptr, 0U, "", "prog");

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() >= 1U);
    CHECK(cmds[0].kind == LoweredKind::BeginRender);
    FakePipe                              pipe;
    const ConstSpan<LoweredCommand>       clist(cmds.data(), cmds.size());
    const ConstSpan<ResolvedBinding>      empty(static_cast<const ResolvedBinding*>(nullptr), 0U);
    CHECK(validate_lowered(ctx, clist, resolve_from_user, &pipe, empty) == ExecuteError::UnsupportedCommand);
    FakeRec rec;
    CHECK(execute_lowered(ctx, clist, rec, resolve_from_user, &pipe, empty) == ExecuteError::UnsupportedCommand);
    CHECK(rec.dispatches == 0); // recorded NOTHING (rejected before any dispatch)
}

TEST_CASE("ceir 14c: a scope with draw + draw_indirect + mesh_dispatch lowers to BeginRender + 3 Draws + EndRender",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 img = declimg(ctx, k, b);
    Value* const                 col = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    Operation* const             sc  = scope_op(ctx, k, b, atts, 1U, 8, 8);
    Block* const                 rb  = scope_body(ctx, sc);
    Value* const                 vc  = konst(ctx, k, rb, 3);
    Value* const                 ic  = konst(ctx, k, rb, 1);
    Value* const                 args = declbuf(ctx, k, rb);
    (void)draw_op(ctx, k, rb, vc, ic, nullptr, 0U, "", "prog"); // render.draw
    {
        Value*           ops[1] = {args};
        Operation* const op     = ctx.create_operation(k.dind, ConstSpan<Value*>(ops, 1U), 0U); // render.draw_indirect
        ctx.set_attr(op, "program", ctx.attr_symbol(StringView("prog")));
        ctx.set_attr(op, "access", ctx.attr_string(StringView("")));
        ctx.set_attr(op, "max_draws", ctx.attr_int(16));
        rb->append(op);
    }
    {
        Value*           ops[3] = {vc, ic, ic};
        Operation* const op     = ctx.create_operation(k.mesh, ConstSpan<Value*>(ops, 3U), 0U); // render.mesh_dispatch
        ctx.set_attr(op, "program", ctx.attr_symbol(StringView("prog")));
        ctx.set_attr(op, "access", ctx.attr_string(StringView("")));
        rb->append(op);
    }
    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() == 5U); // the effect predicate emits Draw for ALL THREE (no draw-op name list in the lowering)
    CHECK(cmds[0].kind == LoweredKind::BeginRender);
    CHECK(cmds[1].kind == LoweredKind::Draw);
    CHECK(cmds[2].kind == LoweredKind::Draw);
    CHECK(cmds[3].kind == LoweredKind::Draw);
    CHECK(cmds[4].kind == LoweredKind::EndRender);
}

TEST_CASE("ceir 14c: draw_indirect lowers preserving max_draws + args identity (the DrawIndex-scar IR half; the push is 14z)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b    = ctx.create_block(0U);
    Value* const                 img  = declimg(ctx, k, b);
    Value* const                 col  = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    Operation* const             sc   = scope_op(ctx, k, b, atts, 1U, 8, 8);
    Block* const                 rb   = scope_body(ctx, sc);
    Value* const                 args = declbuf(ctx, k, rb);
    Value*                       ops[1] = {args};
    Operation* const             di   = ctx.create_operation(k.dind, ConstSpan<Value*>(ops, 1U), 0U);
    ctx.set_attr(di, "program", ctx.attr_symbol(StringView("prog")));
    ctx.set_attr(di, "access", ctx.attr_string(StringView("")));
    ctx.set_attr(di, "max_draws", ctx.attr_int(37)); // ⭐ the DrawIndex RANGE
    rb->append(di);

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    const LoweredCommand* draw_cmd = nullptr;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(cmds.size()); ++i)
    {
        if (cmds[i].kind == LoweredKind::Draw) { draw_cmd = &cmds[i]; break; }
    }
    REQUIRE(draw_cmd != nullptr);
    CHECK(draw_cmd->op == di); // ⭐ the lowered Draw's op back-pointer IS the draw_indirect — the 14z executor reads from here
    // ⭐ PRESERVATION: max_draws (the DrawIndex range) + the %args identity survive lowering — the information the executor
    // needs to push the SV_DrawIndex row is intact.
    const AttrValue md = ctx.attr_value(draw_cmd->op->attr(StringView("max_draws")));
    CHECK(md.kind == AttrKind::Int);
    CHECK(md.i == 37);
    REQUIRE(draw_cmd->op->num_operands() >= 1U);
    CHECK(draw_cmd->op->operand(0U) == args);
    // ⛔ NAMED-FORWARD to CEIR-14z: the executor-PUSHES-the-row assertion + the forced-value probe (render one mesh LARGE,
    // pin the field) — the REN-40 scar warns cheap checks cannot SEE the push failure, so this IR test asserts ONLY that the
    // executor's inputs survive lowering, NEVER that the row is pushed (a lowering test claiming the latter is the false-clean).
}

// ── CEIR-14d: resource-table binding semantics (§156/§157). A ResourceTable is a §157 resident bindless table; at the CEIR
// HOST-orchestration layer it declares + BINDS + hazards like any resource (ceir_is_resource_kind covers it). ⛔ NAMED-
// FORWARD: (a) in-KERNEL table indexing (`table[material_id]`) = CKIR descriptor/nonuniform indexing, not a host CEIR op;
// (b) host-side element extraction → the row where a consumer exists (12a: no vocabulary without a customer); (c) the
// BindingKind ResourceTable enumerator + the type→kind derivation → CEIR-14z/16 (the widen-enum-audit scar pre-flagged, as
// binding.hpp is shared RAF-4). ⛔ NOT mapped onto the deprecated BindlessTextureArray (§156 rejects fixed small arrays).

TEST_CASE("ceir 14d: a resource TABLE binding participates in the precise per-resource hazard (write-T then read-T = RAW on T)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 g   = konst(ctx, k, b, 1);
    Value* const                 tbl = decltable(ctx, k, b); // a §157 bindless resource table
    Value*                       wb[1] = {tbl};
    (void)dispatch_bufs(ctx, k, b, g, wb, 1U, "w", "writer"); // dispatch WRITES table T
    Value*                       rd[1] = {tbl};
    (void)dispatch_bufs(ctx, k, b, g, rd, 1U, "r", "reader"); // dispatch READS table T

    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds);
    REQUIRE(cmds.size() == 3U); // [Dispatch(writer), Barrier(RAW on T), Dispatch(reader)]
    CHECK(cmds[0].kind == LoweredKind::Dispatch);
    CHECK(cmds[1].kind == LoweredKind::Barrier);
    CHECK(cmds[1].hazard == HazardKind::Raw);
    CHECK(cmds[1].resource == ctx.resource_root(tbl)); // ⭐ the TABLE is the conflicting root (PRECISE, not the ambient)
    CHECK(cmds[2].kind == LoweredKind::Dispatch);
}

TEST_CASE("ceir 14d: a table declare + a dispatch binding it verify + round-trip text == builder (the ResourceTable type)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Module* const                m   = ctx.create_module();
    Block* const                 blk = ctx.create_block(0U);
    m->body()->append(blk);
    Value* const g   = konst(ctx, k, blk, 1);
    Value* const tbl = decltable(ctx, k, blk);
    Value*       bd[1] = {tbl};
    (void)dispatch_bufs(ctx, k, blk, g, bd, 1U, "r", "reader"); // a dispatch BINDS the table (a valid resource binding)
    CHECK(ctx.find_dispatch_misuse(*m).kind == DispatchMisuseKind::None); // ⭐ a ResourceTable is an accepted binding

    const crd::containers::String txt = print(ctx, *m, &root);
    Context                       ctx2(&root);
    const Kit                     k2(ctx2);
    const ParseResult             pr = parse(ctx2, StringView(txt.data(), txt.size()));
    REQUIRE(pr.ok);
    const crd::containers::String txt2 = print(ctx2, *pr.module, &root);
    CHECK(StringView(txt.data(), txt.size()) == StringView(txt2.data(), txt2.size())); // the ResourceTable type round-trips
    CHECK(ctx2.find_dispatch_misuse(*pr.module).kind == DispatchMisuseKind::None);
}
