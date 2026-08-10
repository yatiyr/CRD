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
    OpId cst, decl, view, disp, upload;
    explicit Kit(Context& c)
        : cst(c.intern_op("arith", "const")), decl(c.intern_op("resource", "declare")),
          view(c.intern_op("resource", "view")), disp(c.intern_op("compute", "dispatch")),
          upload(c.intern_op("transfer", "upload"))
    {
        (void)arith::register_arith_ops(c);
        (void)func::register_dialect(c);
        (void)resource::register_resource_ops(c);
        (void)compute::register_compute_ops(c);
        (void)transfer::register_transfer_ops(c);
    }
};
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
