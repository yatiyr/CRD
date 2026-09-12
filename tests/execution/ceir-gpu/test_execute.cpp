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
#include <crd/ceir/scene.hpp> // CEIR-17b: the scene dialect (build_resolve_*, type_*, find_scene_misuse) for evaluate_scene_resolve
#include <crd/ceir/exec.hpp> // CEIR-13z-4 leg 1b: the core Interpreter REFUSES a dispatch (typed NoSemantics)
#include <crd/ceir/func.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/gpu/render_materialize.hpp>      // CEIR-14z-1: the render materializers
#include <crd/ceir/gpu/render_fullscreen_build.hpp> // CEIR-16-3b: the fullscreen composite builder
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

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
// CEIR-14z-1: the materializers only STORE the resolved target/program pointers (never DEREF them), so identity-only
// sentinel resolvers suffice — `user` is a void* sentinel round-tripped to the interface pointer (no fake subclass).
crd::gpu::IRasterTarget*  resolve_target(const Operation*, void* user) { return static_cast<crd::gpu::IRasterTarget*>(user); }
// CEIR-16-3c-4: a fake IRasterTarget carrying real dimensions — `extent_from_target` reads width()/height() off the resolved
// target (the sentinel resolver above returns `user`, so a FakeRTarget* passed as `user` round-trips to a sized target).
struct FakeRTarget final : crd::gpu::IRasterTarget
{
    FakeRTarget(crd::u32 w, crd::u32 h) noexcept : m_w(w), m_h(h) {}
    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u32 read_pixel(crd::u32, crd::u32) const noexcept override { return 0U; }

private:
    crd::u32 m_w;
    crd::u32 m_h;
};
crd::gpu::IRasterProgram* resolve_rprog(const Operation*, void* user) { return static_cast<crd::gpu::IRasterProgram*>(user); }
// CEIR-16-2: an IMAGE-typed binding operand resolver (identity sentinel, the same shape) → the draw's SampledTexture.
crd::gpu::ITexture*       resolve_tex(const Value*, void* user) { return static_cast<crd::gpu::ITexture*>(user); }
// CEIR-16-3a-1: a bindless-array resolver. `user` points to a small array-of-fakes descriptor; the resolver returns its
// element pointer + writes the count — the N-ness the materializer stores as texture_array/array_count but never sees in the IR.
struct FakeTexArray
{
    crd::gpu::ITexture* elems[4]{};
    crd::u32            count = 0U;
};
crd::gpu::ITexture* const* resolve_texarray(const Value*, void* user, crd::u32& out_count)
{
    FakeTexArray* const fa = static_cast<FakeTexArray*>(user);
    out_count              = fa->count;
    return fa->elems;
}
// CEIR-16-3b-3: a storage-buffer resolver (identity sentinel) — the constants buffer of the bindless-N fullscreen shape.
crd::gpu::IStorageBuffer* resolve_buf(const Value*, void* user) { return static_cast<crd::gpu::IStorageBuffer*>(user); }
// CEIR-16-mesh-2: a draws resolver (count + per-index item) over the hand-built RasterDrawItem span passed as `user`.
crd::u32 resolve_draws_count(void* user)
{
    return static_cast<crd::u32>(static_cast<crd::containers::ConstSpan<RasterDrawItem>*>(user)->size());
}
void resolve_draws_item(void* user, crd::u32 index, RasterDrawItem& out)
{
    out = (*static_cast<crd::containers::ConstSpan<RasterDrawItem>*>(user))[index];
}
// CEIR-16-mesh-2: an encoder capturing EVERY draw's (command, amplify count, program, binding count) — mesh_dispatch_list
// emits N draws in ONE scope, so a last-only capture cannot check the per-item skip/default/fallback logic.
struct AmpCapEncoder : crd::gpu::ICommandEncoder
{
    int                                                begins = 0, ends = 0;
    crd::containers::Array<crd::gpu::RasterCommandKind> cmds;
    crd::containers::Array<crd::u32>                    counts;
    crd::containers::Array<crd::gpu::IRasterProgram*>   progs;
    crd::containers::Array<crd::usize>                  nbinds;
    explicit AmpCapEncoder(crd::memory::IAllocator* a) : cmds(a), counts(a), progs(a), nbinds(a) {}
    void begin_rendering(const crd::gpu::RenderingDesc&) override { ++begins; }
    void draw(const crd::gpu::RasterDrawPacket& p) override
    {
        cmds.push_back(p.command);
        counts.push_back(p.geometry.kind == crd::gpu::GeometryKind::Meshlet ? p.geometry.group_count_x
                                                                            : p.geometry.patch_count);
        progs.push_back(p.program);
        nbinds.push_back(p.bindings.size());
    }
    void end_rendering() override { ++ends; }
    void dispatch(const crd::gpu::DispatchDesc&) override {}
    void transfer(const crd::gpu::TransferDesc&) override {}
    void trace_rays(const crd::gpu::TraceDesc&) override {}
};
// CEIR-14z-2: a device-free ICommandEncoder that COUNTS + captures the render verbs (the compute/transfer/trace verbs no-op).
struct FakeEncoder : crd::gpu::ICommandEncoder
{
    int                        begins = 0;
    int                        draws  = 0;
    int                        ends   = 0;
    crd::gpu::RenderingDesc     last_rd{};
    crd::gpu::RasterDrawPacket  last_draw{};
    void begin_rendering(const crd::gpu::RenderingDesc& rd) override { ++begins; last_rd = rd; }
    void draw(const crd::gpu::RasterDrawPacket& p) override { ++draws; last_draw = p; }
    void end_rendering() override { ++ends; }
    void dispatch(const crd::gpu::DispatchDesc&) override {}
    void transfer(const crd::gpu::TransferDesc&) override {}
    void trace_rays(const crd::gpu::TraceDesc&) override {}
};

struct Kit
{
    OpId cst, decl, view, disp, upload, col, scope, draw, dind, mesh, mind;
    explicit Kit(Context& c)
        : cst(c.intern_op("arith", "const")), decl(c.intern_op("resource", "declare")),
          view(c.intern_op("resource", "view")), disp(c.intern_op("compute", "dispatch")),
          upload(c.intern_op("transfer", "upload")), col(c.intern_op("render", "color_attachment")),
          scope(c.intern_op("render", "scope")), draw(c.intern_op("render", "draw")),
          dind(c.intern_op("render", "draw_indirect")), mesh(c.intern_op("render", "mesh_dispatch")),
          mind(c.intern_op("render", "mesh_dispatch_indirect"))
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
// CEIR-16-3a-2: an image resource declared with an explicit binding `slot` attr (the shadow-atlas tex@4 shape) — the
// materializer reads it off the defining op, overriding the ordinal position.
Value* declimg_at(Context& c, const Kit& k, Block* b, i64 slot)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_image(ImageDim::Dim2D, c.type_f32()));
    c.set_attr(d, "slot", c.attr_int(slot));
    b->append(d);
    return d->result(0U);
}
// CEIR-16-3a-3: a sampler resource declared at an explicit slot — `comparison` selects the CEIR Sampler type's is_signed
// (a shadow COMPARISON sampler vs a plain filtering one). The materializer maps it to a standalone Comparison/Sampler binding.
Value* declsamp_at(Context& c, const Kit& k, Block* b, i64 slot, bool comparison)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_sampler(comparison));
    c.set_attr(d, "slot", c.attr_int(slot));
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
// CEIR-16-3a-1: a graph-owned RESOURCE TABLE of an IMAGE element — the bindless-texture-array binding shape (the fullscreen
// composite's n>1 / blend-load arms). The render materializer maps this table TYPE to a BindlessTextureArray binding.
Value* decltblimg(Context& c, const Kit& k, Block* b)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_resource_table(c.type_image(ImageDim::Dim2D, c.type_f32())));
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;

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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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
    crd::memory::GrowableTlsfAllocator root;
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

// ── CEIR-14z-1: the render MATERIALIZERS (device-free) — CEIR op → command_model desc, Option A (test-surface bridge). ──

TEST_CASE("ceir 14z-1: materialize_rendering_desc maps the scope + attachments (RAH-1a.1: uint clear = ClearKind::Uint)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    int                          tgt_sentinel = 0;
    void* const                  tgt_u        = &tgt_sentinel; // an identity sentinel (never deref'd)
    const OpId                   depk = ctx.intern_op("render", "depth_attachment");
    Block* const                 b    = ctx.create_block(0U);

    Value* const     uimg = declimg(ctx, k, b); // color 0: a uint-clear id target (the RAH-1a.1 thread)
    Value*           uo[1] = {uimg};
    Operation* const uc = ctx.create_operation(k.col, ConstSpan<Value*>(uo, 1U), 1U, render::type_color_attachment(ctx, uimg->type()));
    ctx.set_attr(uc, "clear_kind", ctx.attr_string(StringView("uint")));
    ctx.set_attr(uc, "clear_uint", ctx.attr_int(7));
    ctx.set_attr(uc, "load", ctx.attr_string(StringView("clear")));
    b->append(uc);
    Value* const     fimg = declimg(ctx, k, b); // color 1: a float clear + alpha blend
    Value*           fo[1] = {fimg};
    Operation* const fc = ctx.create_operation(k.col, ConstSpan<Value*>(fo, 1U), 1U, render::type_color_attachment(ctx, fimg->type()));
    ctx.set_attr(fc, "clear_r", ctx.attr_float(0.25));
    ctx.set_attr(fc, "blend", ctx.attr_string(StringView("alpha")));
    b->append(fc);
    Value* const     dimg = declimg(ctx, k, b); // depth
    Value*           dvo[1] = {dimg};
    Operation* const dp = ctx.create_operation(depk, ConstSpan<Value*>(dvo, 1U), 1U, render::type_depth_attachment(ctx, dimg->type()));
    ctx.set_attr(dp, "compare", ctx.attr_string(StringView("greater")));
    ctx.set_attr(dp, "clear_depth", ctx.attr_float(0.5));
    b->append(dp);

    Value*           atts[3] = {uc->result(0U), fc->result(0U), dp->result(0U)};
    Operation* const sc      = scope_op(ctx, k, b, atts, 3U, 1920, 1080);
    ctx.set_attr(sc, "sample_count", ctx.attr_int(4));

    crd::gpu::RenderingDesc rd;
    REQUIRE(materialize_rendering_desc(ctx, sc, resolve_target, tgt_u, rd));
    CHECK(rd.width == 1920U);
    CHECK(rd.height == 1080U);
    CHECK(rd.sample_count == 4U);
    REQUIRE(rd.color.size() == 2U); // the depth attachment does NOT go in color[]
    CHECK(rd.color[0].clear_kind == crd::gpu::ClearKind::Uint); // ⭐ RAH-1a.1 end-to-end
    CHECK(rd.color[0].clear_uint == 7U);
    CHECK(rd.color[0].target == static_cast<crd::gpu::IRasterTarget*>(tgt_u));
    CHECK(rd.color[0].load == crd::gpu::LoadOp::Clear);
    CHECK(rd.color[1].clear_kind == crd::gpu::ClearKind::Float);
    CHECK(rd.color[1].clear.r == 0.25F);
    CHECK(rd.color[1].blend == crd::gpu::BlendMode::Alpha);
    CHECK(rd.depth.enabled);
    CHECK(rd.depth.compare == crd::gpu::DepthCompare::Greater);
    CHECK(rd.depth.clear_depth == 0.5F);
}

TEST_CASE("ceir 16d-live-4a-1: the FULL BlendMode vocabulary (incl. the WBOIT MRT modes) verifies + materializes",
          "[ceir][ceir-gpu][render]")
{
    // The CEIR `blend` vocabulary was closed at 4 (opaque/alpha/additive/premultiplied) — the WBOIT MRT modes
    // (multiply / revealage_multiply / reveal_composite) had NO string, so a deferred/WBOIT scene pass's reveal
    // attachment silently materialized Opaque. 4a-1 opens the closed set to the whole BlendMode enum. This proves BOTH
    // directions: the verifier (find_render_misuse / kBlendVocab) ACCEPTS each of the 7, blend_of maps each to its enum,
    // and a garbage string is REJECTED (BlendInvalid) — the gate the mrt>=2 representation (4a-2/3) depends on.
    crd::memory::GrowableTlsfAllocator root;
    int                          tgt = 0;
    const auto                   check_blend = [&](StringView bstr, crd::gpu::BlendMode expect, bool valid) {
        Context      ctx(&root);
        const Kit    k(ctx);
        Module* const m = ctx.create_module();
        Block*        b = m->body()->first_block();
        if (b == nullptr)
        {
            b = ctx.create_block(0U);
            m->body()->append(b);
        }
        Value* const     img   = declimg(ctx, k, b);
        Value*           fo[1]  = {img};
        Operation* const fc     = ctx.create_operation(k.col, ConstSpan<Value*>(fo, 1U), 1U,
                                                       render::type_color_attachment(ctx, img->type()));
        ctx.set_attr(fc, "blend", ctx.attr_string(bstr));
        b->append(fc);
        Value*           atts[1] = {fc->result(0U)};
        Operation* const sc      = scope_op(ctx, k, b, atts, 1U, 8, 8);
        const auto       misuse  = ctx.find_render_misuse(*m);
        if (valid)
        {
            CHECK(misuse.kind != crd::ceir::RenderMisuseKind::BlendInvalid); // the verifier ACCEPTS the blend
            crd::gpu::RenderingDesc rd;
            REQUIRE(materialize_rendering_desc(ctx, sc, resolve_target, &tgt, rd));
            REQUIRE(rd.color.size() == 1U);
            CHECK(rd.color[0].blend == expect); // blend_of maps the string to the enum
        }
        else
        {
            CHECK(misuse.kind == crd::ceir::RenderMisuseKind::BlendInvalid); // a non-vocab string is REJECTED
        }
    };
    check_blend(StringView("opaque"), crd::gpu::BlendMode::Opaque, true);
    check_blend(StringView("alpha"), crd::gpu::BlendMode::Alpha, true);
    check_blend(StringView("additive"), crd::gpu::BlendMode::Additive, true);
    check_blend(StringView("premultiplied"), crd::gpu::BlendMode::PremultipliedAlpha, true);
    check_blend(StringView("multiply"), crd::gpu::BlendMode::Multiply, true);                     // ⭐ new
    check_blend(StringView("revealage_multiply"), crd::gpu::BlendMode::RevealageMultiply, true);  // ⭐ new (WBOIT reveal)
    check_blend(StringView("reveal_composite"), crd::gpu::BlendMode::RevealComposite, true);      // ⭐ new
    check_blend(StringView("bogus_blend"), crd::gpu::BlendMode::Opaque, false);                   // ⛔ rejected
}

TEST_CASE("ceir 16d-live-4a-2: build_scene_ceir emits an N-colour MRT scope with per-attachment blend",
          "[ceir][ceir-gpu][render]")
{
    // The mrt≥2 scope (a deferred G-buffer / WBOIT accumulate) = the SAME render.scope + render.scene_draw_list, but with N
    // color_attachment ops each carrying its `blend`. 4a-2 proves the BUILDER: build_scene_ceir(mrt_n=2) VERIFIES
    // (find_render_misuse accepts a ≥2-colour scope containing scene_draw_list — GAP ii) and MATERIALIZES to 2 attachments
    // with the right blends (Additive accum + RevealageMultiply reveal, the WBOIT pair) all LoadOp::Clear (legacy MRT arm).
    crd::memory::GrowableTlsfAllocator  root;
    Context                       ctx(&root);
    Array<LoweredCommand>         plan(&root);
    SceneBuildDesc                bd;
    bd.has_color = true;
    bd.has_depth = false;
    bd.mrt_n     = 2U;
    bd.blend[0]  = crd::gpu::BlendMode::Additive;          // color0 = accum
    bd.blend[1]  = crd::gpu::BlendMode::RevealageMultiply; // color1 = revealage (the mode no generic blend expresses)
    REQUIRE(build_scene_ceir(ctx, bd, plan)); // ⭐ GAP ii: find_render_misuse accepts the 2-colour scope + scene_draw_list

    REQUIRE(plan.size() >= 1U);
    FakeRTarget             tgt(64U, 64U);
    crd::gpu::RenderingDesc rd;
    REQUIRE(materialize_rendering_desc(ctx, plan[0].op, resolve_target, &tgt, rd));
    REQUIRE(rd.color.size() == 2U); // ⭐ TWO colour attachments materialized
    CHECK(rd.color[0].blend == crd::gpu::BlendMode::Additive);
    CHECK(rd.color[1].blend == crd::gpu::BlendMode::RevealageMultiply);
    CHECK(rd.color[0].load == crd::gpu::LoadOp::Clear); // the MRT arm hardcodes Clear on every attachment
    CHECK(rd.color[1].load == crd::gpu::LoadOp::Clear);

    // ⛔ the SINGLE-colour path stays byte-identical (mrt_n default 1 ⇒ NO blend attr ⇒ Opaque, honours load).
    Context               ctx1(&root);
    Array<LoweredCommand> plan1(&root);
    SceneBuildDesc        single; // mrt_n=1 default
    REQUIRE(build_scene_ceir(ctx1, single, plan1));
    crd::gpu::RenderingDesc rd1;
    REQUIRE(materialize_rendering_desc(ctx1, plan1[0].op, resolve_target, &tgt, rd1));
    REQUIRE(rd1.color.size() == 1U);
    CHECK(rd1.color[0].blend == crd::gpu::BlendMode::Opaque);
}

TEST_CASE("ceir 16z-1: build_scene_ceir emits a UINT typed clear (the visbuffer id target, RAH-1a.1)",
          "[ceir][ceir-gpu][render]")
{
    // A visbuffer pass (dissolved onto scene.raster, §41) writes an R32_UINT id target whose typed clear IS the whole
    // semantics. 16z-1 proves the BUILDER: build_scene_ceir(clear_is_uint) VERIFIES (find_render_misuse — the colour image is
    // UINT-format, so the RAH-1a.1 ClearKindFormatMismatch scar does NOT fire) and MATERIALIZES to ClearKind::Uint + the id.
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Array<LoweredCommand>        plan(&root);
    SceneBuildDesc               bd;
    bd.has_color     = true;
    bd.has_depth     = false;
    bd.clear_is_uint = true;
    bd.clear_uint    = 7U; // the background id (the shipped scene_visbuffer asset's clear_id)
    REQUIRE(build_scene_ceir(ctx, bd, plan)); // ⭐ verifies: a uint clear on a uint-format attachment is accepted

    REQUIRE(plan.size() >= 1U);
    FakeRTarget             tgt(64U, 64U);
    crd::gpu::RenderingDesc rd;
    REQUIRE(materialize_rendering_desc(ctx, plan[0].op, resolve_target, &tgt, rd));
    REQUIRE(rd.color.size() == 1U);
    CHECK(rd.color[0].clear_kind == crd::gpu::ClearKind::Uint); // ⭐ the typed clear
    CHECK(rd.color[0].clear_uint == 7U);
    CHECK(rd.color[0].load == crd::gpu::LoadOp::Clear); // clears the id (later draws load — encoder derives it)

    // ⛔ the FLOAT path stays byte-identical (clear_is_uint default false ⇒ ClearKind::Float + the float clears).
    Context               ctxf(&root);
    Array<LoweredCommand> planf(&root);
    SceneBuildDesc        flt;
    flt.clear = crd::gpu::ClearColor{0.25F, 0.0F, 0.0F, 1.0F};
    REQUIRE(build_scene_ceir(ctxf, flt, planf));
    crd::gpu::RenderingDesc rdf;
    REQUIRE(materialize_rendering_desc(ctxf, planf[0].op, resolve_target, &tgt, rdf));
    REQUIRE(rdf.color.size() == 1U);
    CHECK(rdf.color[0].clear_kind == crd::gpu::ClearKind::Float);
    CHECK(rdf.color[0].clear.r == 0.25F);
}

TEST_CASE("ceir 16-3c-4: extent_from_target overrides the scope width/height with the RESOLVED target size",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 img = declimg(ctx, k, b);
    Value* const                 col = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    FakeRTarget                  tgt(640U, 480U); // the resolved target the scope binds to

    SECTION("extent_from_target set -> the render area is the target's 640x480, not the 1x1 placeholder")
    {
        Operation* const sc = scope_op(ctx, k, b, atts, 1U, 1, 1); // a 1x1 placeholder (as the fullscreen builder emits)
        ctx.set_attr(sc, "extent_from_target", ctx.attr_bool(true));
        crd::gpu::RenderingDesc rd;
        REQUIRE(materialize_rendering_desc(ctx, sc, resolve_target, &tgt, rd));
        CHECK(rd.width == 640U); // ⭐ the resolved target's size (record_fullscreen_raster parity), NOT the 1x1 attr
        CHECK(rd.height == 480U);
    }
    SECTION("absent -> the authored width/height win (backward-compatible)")
    {
        Operation* const sc = scope_op(ctx, k, b, atts, 1U, 8, 8); // no extent_from_target → static 8x8
        crd::gpu::RenderingDesc rd;
        REQUIRE(materialize_rendering_desc(ctx, sc, resolve_target, &tgt, rd));
        CHECK(rd.width == 8U);
        CHECK(rd.height == 8U);
    }
}

TEST_CASE("ceir 14z-1: materialize_draw_packet maps command + GeometryKind + counts per draw op", "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    int                          prog_sentinel = 0;
    void* const                  prog_u = &prog_sentinel; // an identity sentinel (never deref'd)
    Block* const                 b  = ctx.create_block(0U);
    Value* const                 vc = konst(ctx, k, b, 3);
    Value* const                 ic = konst(ctx, k, b, 1);

    SECTION("render.draw with 0 bindings -> Draw / None (procedural)")
    {
        Operation* const d = draw_op(ctx, k, b, vc, ic, nullptr, 0U, "", "prog");
        ctx.set_attr(d, "first_vertex", ctx.attr_int(5));
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, d, RenderResolvers{.program = resolve_rprog, .program_user = prog_u}, p));
        CHECK(p.program == static_cast<crd::gpu::IRasterProgram*>(prog_u));
        CHECK(p.command == crd::gpu::RasterCommandKind::Draw);
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::None); // 0 bindings ⇒ procedural (the fullscreen/proc-triangle path)
        CHECK(p.geometry.vertex_or_index_count == 3U);
        CHECK(p.geometry.instance_count == 1U);
        CHECK(p.geometry.first_vertex == 5U);
    }
    SECTION("render.draw with a vertex-pull binding -> Draw / StoragePull")
    {
        Value* const     vbuf = declbuf(ctx, k, b);
        Value*           bd[1] = {vbuf};
        Operation* const d = draw_op(ctx, k, b, vc, ic, bd, 1U, "r", "prog");
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, d, RenderResolvers{.program = resolve_rprog, .program_user = prog_u}, p));
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::StoragePull); // ≥1 binding ⇒ vertex-pull
    }
    SECTION("CEIR-16-2: an IMAGE binding operand -> a SampledTexture binding (kind derives from the operand TYPE)")
    {
        Value* const     img   = declimg(ctx, k, b); // an IMAGE-typed binding operand (vs declbuf's buffer)
        Value*           bd[1] = {img};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 1U, "r", "prog");
        int              tex_sentinel = 0;
        void* const      tex_u = &tex_sentinel;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .texture = resolve_tex, .texture_user = tex_u},
            p));
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::SampledTexture); // ⭐ Image type ⇒ sampled texture, NOT StorageBuffer
        CHECK(p.bindings[0].frequency == crd::gpu::BindingFrequency::Material);
        CHECK(p.bindings[0].slot == 0U);                                        // no explicit `slot` attr ⇒ the ordinal (0)
        CHECK(p.bindings[0].texture == static_cast<crd::gpu::ITexture*>(tex_u)); // resolved through resolvers.texture
        CHECK(p.bindings[0].buffer == nullptr);                                  // an image binding sets .texture, never .buffer
    }
    SECTION("CEIR-16-2: a BUFFER binding with ONLY a texture resolver -> typed fail (dispatch keys on TYPE, not the resolver set)")
    {
        Value* const     buf   = declbuf(ctx, k, b);
        Value*           bd[1] = {buf};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 1U, "r", "prog");
        int              tex_sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        // a texture resolver is set (so the tail loop runs) but the operand is a BUFFER → resolvers.storage is null → false,
        // never a silent skip and never wrongly texture-resolved.
        CHECK(!materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .texture = resolve_tex, .texture_user = &tex_sentinel},
            p));
    }
    SECTION("CEIR-16-3a-1: a RESOURCE-TABLE-of-image binding operand -> a BindlessTextureArray (count behind the resolver)")
    {
        Value* const     tbl   = decltblimg(ctx, k, b); // a resource_table(image) binding operand (the bindless shape)
        Value*           bd[1] = {tbl};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 1U, "r", "prog");
        // the materializer stores the array POINTER + count (never inspects the elements), so leaving the fake elems null and
        // asserting the pointer/count rode through fully exercises the dispatch.
        FakeTexArray     fa;
        fa.count = 2U;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, d,
            RenderResolvers{
                .program = resolve_rprog, .program_user = prog_u, .texture_array = resolve_texarray, .texture_array_user = &fa},
            p));
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::BindlessTextureArray); // ⭐ table-of-image type ⇒ bindless array
        CHECK(p.bindings[0].frequency == crd::gpu::BindingFrequency::Material);
        CHECK(p.bindings[0].array_count == 2U);        // the N-ness the resolver wrote — never in the IR
        CHECK(p.bindings[0].texture_array == fa.elems); // the resolved array pointer rode through
        CHECK(p.bindings[0].texture == nullptr);        // a bindless array sets texture_array, not the single texture
    }
    SECTION("CEIR-16-3a-1: a table-of-image binding but NO array resolver -> typed fail (never a silent skip)")
    {
        Value* const     tbl   = decltblimg(ctx, k, b);
        Value*           bd[1] = {tbl};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 1U, "r", "prog");
        int              tex_sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        // a texture (single) resolver is set so the loop runs, but the operand is a resource-TABLE → needs texture_array → false.
        CHECK(!materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .texture = resolve_tex, .texture_user = &tex_sentinel},
            p));
    }
    SECTION("CEIR-16-3a-2: an explicit `slot` attr on a binding's defining op overrides the ordinal position")
    {
        Value* const     img   = declimg_at(ctx, k, b, 4); // an image declared at binding slot 4 (the shadow-atlas tex slot)
        Value*           bd[1] = {img};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 1U, "r", "prog");
        int              tex_sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .texture = resolve_tex, .texture_user = &tex_sentinel},
            p));
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.bindings[0].slot == 4U); // ⭐ the explicit slot, NOT the ordinal 0
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::SampledTexture);
    }
    SECTION("CEIR-16-3a-3: the shadow-atlas shape — depth texture @slot4 + a COMPARISON sampler @slot5 (bind_atlas parity)")
    {
        Value* const     tex   = declimg_at(ctx, k, b, 4);        // the depth atlas texture at slot 4
        Value* const     samp  = declsamp_at(ctx, k, b, 5, true); // a COMPARISON sampler at slot 5
        Value*           bd[2] = {tex, samp};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 2U, "r", "prog");
        int              tex_sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .texture = resolve_tex, .texture_user = &tex_sentinel},
            p));
        REQUIRE(p.bindings.size() == 2U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::SampledTexture);    // slot-4 depth texture (resolved)
        CHECK(p.bindings[0].slot == 4U);
        CHECK(p.bindings[1].kind == crd::gpu::BindingKind::ComparisonSampler); // ⭐ slot-5 comparison sampler (the moment-shadow bit)
        CHECK(p.bindings[1].slot == 5U);
        CHECK(p.bindings[1].texture == nullptr);                              // a sampler binding carries NO texture
    }
    SECTION("CEIR-16-3a-3: a PLAIN sampler operand -> Sampler, NOT comparison (the moment/variance atlas — REN-40-D bit)")
    {
        Value* const     tex   = declimg_at(ctx, k, b, 4);
        Value* const     samp  = declsamp_at(ctx, k, b, 5, false); // a PLAIN (non-comparison) sampler at slot 5
        Value*           bd[2] = {tex, samp};
        Operation* const d     = draw_op(ctx, k, b, vc, ic, bd, 2U, "r", "prog");
        int              tex_sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .texture = resolve_tex, .texture_user = &tex_sentinel},
            p));
        REQUIRE(p.bindings.size() == 2U);
        CHECK(p.bindings[1].kind == crd::gpu::BindingKind::Sampler); // ⭐ is_signed=false ⇒ PLAIN filtering sampler, not comparison
    }
    SECTION("render.draw_indirect -> DrawIndirect / Indirect, max_draws survives")
    {
        Value* const     args = declbuf(ctx, k, b);
        Value*           ops[1] = {args};
        Operation* const d = ctx.create_operation(k.dind, ConstSpan<Value*>(ops, 1U), 0U);
        ctx.set_attr(d, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(d, "access", ctx.attr_string(StringView("")));
        ctx.set_attr(d, "max_draws", ctx.attr_int(42));
        b->append(d);
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, d, RenderResolvers{.program = resolve_rprog, .program_user = prog_u}, p));
        CHECK(p.command == crd::gpu::RasterCommandKind::DrawIndirect);
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::Indirect);
        CHECK(p.geometry.max_draws == 42U); // ⭐ the DrawIndex range survives into the packet (REN-40; push at 14z-6)
    }
    SECTION("render.mesh_dispatch -> DispatchMesh / Meshlet, group counts fold")
    {
        Value*           ops[3] = {vc, ic, ic};
        Operation* const d = ctx.create_operation(k.mesh, ConstSpan<Value*>(ops, 3U), 0U);
        ctx.set_attr(d, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(d, "access", ctx.attr_string(StringView("")));
        b->append(d);
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, d, RenderResolvers{.program = resolve_rprog, .program_user = prog_u}, p));
        CHECK(p.command == crd::gpu::RasterCommandKind::DispatchMesh);
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::Meshlet);
        CHECK(p.geometry.group_count_x == 3U);
        CHECK(p.geometry.group_count_y == 1U);
    }
    SECTION("render.mesh_dispatch with a y/z > 1 grid is REFUSED (the Meshlet verb is 1D — LOUD, not a silent narrow)")
    {
        // ⛔ CEIR-14z-7: the op is 3D but every Meshlet verb consumes group_count_x only, so a (gx, gy>1, gz) would silently
        // draw (gx). materialize_draw_packet must FAIL (→ UnsupportedCommand) rather than lower a program that draws the wrong thing.
        Value*           ops[3] = {vc, vc, ic}; // grid (3, 3, 1) — group_count_y = 3 != 1
        Operation* const d = ctx.create_operation(k.mesh, ConstSpan<Value*>(ops, 3U), 0U);
        ctx.set_attr(d, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(d, "access", ctx.attr_string(StringView("")));
        b->append(d);
        crd::gpu::RasterDrawPacket p;
        CHECK(!materialize_draw_packet(ctx, d, RenderResolvers{.program = resolve_rprog, .program_user = prog_u}, p));
    }
    SECTION("render.mesh_dispatch_indirect -> DispatchMeshIndirect / MeshletIndirect, args buffer RESOLVED (CEIR-16-mesh-1)")
    {
        // ⛔ CEIR-16-mesh-1: MeshletIndirect reads its group counts from the %args buffer (operand 0). The materializer set
        // only args_OFFSET (never the buffer) — a mesh-indirect draw dispatched off a NULL args buffer. A storage resolver
        // must land the buffer on g.args_buffer (record_mesh_indirect parity: p.geometry.args_buffer = ctx.storage("args")).
        char             arg_sentinel;
        void* const      arg_u  = &arg_sentinel;
        Value* const     args   = declbuf(ctx, k, b);
        Value*           ops[1] = {args};
        Operation* const d      = ctx.create_operation(k.mind, ConstSpan<Value*>(ops, 1U), 0U);
        ctx.set_attr(d, "program", ctx.attr_symbol(StringView("p")));
        ctx.set_attr(d, "access", ctx.attr_string(StringView("")));
        ctx.set_attr(d, "args_offset", ctx.attr_int(64));
        b->append(d);
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, d,
            RenderResolvers{.program = resolve_rprog, .program_user = prog_u, .storage = resolve_buf, .storage_user = arg_u},
            p));
        CHECK(p.command == crd::gpu::RasterCommandKind::DispatchMeshIndirect);
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::MeshletIndirect);
        CHECK(p.geometry.args_buffer == static_cast<crd::gpu::IStorageBuffer*>(arg_u)); // ⭐ the fix — was null before
        CHECK(p.geometry.args_offset == 64U);
    }
}

TEST_CASE("ceir 16-3b-1: build_fullscreen_ceir emits a verified, lowerable procedural/plain composite", "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator root;
    SECTION("procedural (n=0 reads) -> BeginRender, Draw, EndRender; the draw is geometry-None with no bindings")
    {
        Context               ctx(&root); // a FRESH context (the builder registers its dialects)
        FullscreenBuildDesc   desc;        // num_inputs defaults to 0 -> the procedural clear/blit shape
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan)); // returns true ⇒ find_render_misuse passed internally
        REQUIRE(plan.size() == 3U);
        CHECK(plan[0].kind == LoweredKind::BeginRender);
        CHECK(plan[1].kind == LoweredKind::Draw);
        CHECK(plan[2].kind == LoweredKind::EndRender);
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int                       sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, draw, RenderResolvers{.program = resolve_rprog, .program_user = &sentinel}, p));
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::None); // ⭐ a fullscreen draw is PROCEDURAL (VS reads gl_VertexIndex)
        CHECK(p.bindings.size() == 0U);
    }
    SECTION("plain single read -> ONE SampledTexture binding at slot 0 with its source-param identity attr")
    {
        // ⛔ the PLAIN shape is n==1: record_fullscreen_raster binds >1 read as a BINDLESS array (tested below), never N
        // separate SampledTextures, so the builder routes n>1 to the bindless shape and plain handles the single read.
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs             = 1U;
        desc.inputs[0].source_param = 111U; // pass_param_id("input0") stand-in
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan));
        REQUIRE(plan.size() == 3U);
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int                       sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, draw,
            RenderResolvers{.program = resolve_rprog, .program_user = &sentinel, .texture = resolve_tex, .texture_user = &sentinel},
            p));
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::None); // ⭐ 3b-1b: PROCEDURAL despite the texture binding (geometry attr)
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::SampledTexture);
        CHECK(p.bindings[0].slot == 0U);
        // ⭐ advisor constraint #2: the binding's defining op carries its SOURCE-param identity (distinct from the slot),
        // so the record-time resolver can map operand → RecordContext.texture(source_param).
        const AttrId s0 = draw->operand(2U)->defining_op()->attr(StringView("source"));
        REQUIRE(s0.valid());
        CHECK(ctx.attr_value(s0).i == 111);
    }
    SECTION("a desc with > 8 reads is rejected (the fixed input0..input7 fan)")
    {
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs = 9U;
        Array<LoweredCommand> plan(&root);
        CHECK(!build_fullscreen_ceir(ctx, desc, plan));
    }
    SECTION("atlas shape (n=1 depth, !depth_as_float) -> SampledTexture@4 + ComparisonSampler@5 (bind_atlas parity, REN-40-D)")
    {
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs             = 1U;
        desc.inputs[0].source_param = 444U;
        desc.inputs[0].is_depth     = true;
        desc.depth_as_float         = false;
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan));
        REQUIRE(plan.size() == 3U);
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int                       sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, draw,
            RenderResolvers{.program = resolve_rprog, .program_user = &sentinel, .texture = resolve_tex, .texture_user = &sentinel},
            p));
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::None);
        REQUIRE(p.bindings.size() == 2U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::SampledTexture);    // the depth atlas texture
        CHECK(p.bindings[0].slot == 4U);
        CHECK(p.bindings[1].kind == crd::gpu::BindingKind::ComparisonSampler); // ⭐ the slot-5 comparison sampler companion
        CHECK(p.bindings[1].slot == 5U);
        CHECK(p.bindings[1].texture == nullptr);
    }
    SECTION("a depth read WITH depth_as_float stays PLAIN (the moment_convert / HZB raw-depth read, slot 0 not the atlas)")
    {
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs             = 1U;
        desc.inputs[0].source_param = 555U;
        desc.inputs[0].is_depth     = true;
        desc.depth_as_float         = true; // ⛔⛔ REN-40-D: a raw-depth FLOAT read (plain sampler), NOT the comparison atlas
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan));
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int                       sentinel = 0;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(
            ctx, draw,
            RenderResolvers{.program = resolve_rprog, .program_user = &sentinel, .texture = resolve_tex, .texture_user = &sentinel},
            p));
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::SampledTexture); // a plain single texture...
        CHECK(p.bindings[0].slot == 0U);                                    // ...at the ordinal slot 0, NOT the atlas slot 4
    }
    SECTION("bindless-N (n>1) -> ONE BindlessTextureArray binding at slot 0 (the fan collapses to one IR binding)")
    {
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs             = 3U;
        desc.inputs[0].source_param = 10U;
        desc.inputs[1].source_param = 20U;
        desc.inputs[2].source_param = 30U;
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan));
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int          sentinel = 0;
        FakeTexArray fa;
        fa.count = 3U;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, draw,
                                        RenderResolvers{.program            = resolve_rprog,
                                                        .program_user       = &sentinel,
                                                        .texture_array      = resolve_texarray,
                                                        .texture_array_user = &fa},
                                        p));
        CHECK(p.geometry.kind == crd::gpu::GeometryKind::None);
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::BindlessTextureArray); // ⭐ the whole fan is ONE IR binding
        CHECK(p.bindings[0].slot == 0U);
        CHECK(p.bindings[0].array_count == 3U);
    }
    SECTION("bindless-N with a constants buffer -> BindlessTextureArray + a StorageBuffer (the TAA-resolve shape)")
    {
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs             = 2U;
        desc.inputs[0].source_param = 10U;
        desc.inputs[1].source_param = 20U;
        desc.constants_param        = 99U;
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan));
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int          sentinel = 0;
        FakeTexArray fa;
        fa.count = 2U;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, draw,
                                        RenderResolvers{.program            = resolve_rprog,
                                                        .program_user       = &sentinel,
                                                        .storage            = resolve_buf,
                                                        .storage_user       = &sentinel,
                                                        .texture_array      = resolve_texarray,
                                                        .texture_array_user = &fa},
                                        p));
        REQUIRE(p.bindings.size() == 2U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::BindlessTextureArray);
        CHECK(p.bindings[1].kind == crd::gpu::BindingKind::StorageBuffer); // ⭐ the constants buffer
        CHECK(p.bindings[1].slot == 0U); // slot 0 in the Object set (distinct from the array's Material slot 0)
    }
    SECTION("bindless-blend-load (n=1, load + non-opaque blend) -> ONE BindlessTextureArray (the WBOIT resolve shape)")
    {
        Context             ctx(&root);
        FullscreenBuildDesc desc;
        desc.num_inputs             = 1U;
        desc.inputs[0].source_param = 7U;
        desc.load                   = true;
        desc.blend                  = crd::gpu::BlendMode::Alpha; // load + blend ⇒ bindless-of-one (NOT a plain SampledTexture)
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_fullscreen_ceir(ctx, desc, plan));
        const Operation* const draw = plan[1].op;
        REQUIRE(draw != nullptr);
        int          sentinel = 0;
        FakeTexArray fa;
        fa.count = 1U;
        crd::gpu::RasterDrawPacket p;
        REQUIRE(materialize_draw_packet(ctx, draw,
                                        RenderResolvers{.program            = resolve_rprog,
                                                        .program_user       = &sentinel,
                                                        .texture_array      = resolve_texarray,
                                                        .texture_array_user = &fa},
                                        p));
        REQUIRE(p.bindings.size() == 1U);
        CHECK(p.bindings[0].kind == crd::gpu::BindingKind::BindlessTextureArray); // ⭐ bindless-of-ONE, not a plain SampledTexture
        CHECK(p.bindings[0].array_count == 1U);
    }
}

TEST_CASE("ceir 16-mesh-1: build_mesh_indirect_ceir emits a verified, lowerable mesh-indirect composite",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root); // a FRESH context (the builder registers its dialects)
    MeshIndirectBuildDesc        desc;
    desc.args_param  = 222U; // pass_param_id("args") stand-in
    desc.args_offset = 128U;
    desc.clear       = {0.1F, 0.2F, 0.3F, 1.0F};
    Array<LoweredCommand> plan(&root);
    REQUIRE(build_mesh_indirect_ceir(ctx, desc, plan)); // true ⇒ find_render_misuse passed internally
    REQUIRE(plan.size() == 3U);
    CHECK(plan[0].kind == LoweredKind::BeginRender);
    CHECK(plan[1].kind == LoweredKind::Draw);
    CHECK(plan[2].kind == LoweredKind::EndRender);

    // the BeginRender carries the CLEAR colour — a mesh dispatch can leave uncovered pixels (record_mesh_indirect parity),
    // unlike the fullscreen triangle that overwrote every pixel, so the clear colour must survive into the RenderingDesc.
    FakeRTarget             tgt(320U, 240U); // extent_from_target ⇒ the materializer calls target->width(): a REAL target, not a sentinel
    crd::gpu::RenderingDesc rd;
    REQUIRE(materialize_rendering_desc(ctx, plan[0].op, resolve_target, &tgt, rd));
    REQUIRE(rd.color.size() == 1U);
    CHECK(rd.color[0].load == crd::gpu::LoadOp::Clear);
    CHECK(rd.color[0].clear_kind == crd::gpu::ClearKind::Float);
    CHECK(rd.color[0].clear.r > 0.09F);
    CHECK(rd.color[0].clear.r < 0.11F); // ⭐ 0.1 carried (not the fullscreen default 0)

    // the Draw materializes to DispatchMeshIndirect with the %args buffer RESOLVED at operand 0 + its offset (the mesh-1 fix).
    const Operation* const draw = plan[1].op;
    REQUIRE(draw != nullptr);
    int                       prog_sentinel = 0;
    char                      arg_sentinel;
    void* const               arg_u = &arg_sentinel;
    crd::gpu::RasterDrawPacket p;
    REQUIRE(materialize_draw_packet(
        ctx, draw,
        RenderResolvers{
            .program = resolve_rprog, .program_user = &prog_sentinel, .storage = resolve_buf, .storage_user = arg_u},
        p));
    CHECK(p.command == crd::gpu::RasterCommandKind::DispatchMeshIndirect);
    CHECK(p.geometry.kind == crd::gpu::GeometryKind::MeshletIndirect);
    CHECK(p.geometry.args_buffer == static_cast<crd::gpu::IStorageBuffer*>(arg_u)); // ⛔ operand-0 args buffer resolved
    CHECK(p.geometry.args_offset == 128U);
    CHECK(p.bindings.size() == 0U); // record_mesh_indirect binds NO descriptors
    // ⭐ advisor constraint #2: the %args operand carries its SOURCE-param identity (resolver → RecordContext.storage).
    const AttrId s = draw->operand(0U)->defining_op()->attr(StringView("source"));
    REQUIRE(s.valid());
    CHECK(ctx.attr_value(s).i == 222);
}

TEST_CASE("ceir 16-mesh-2: execute_render_lowered EXPANDS mesh_dispatch_list over the host DrawList (skip/default/fallback)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator     root;
    int                              def_s  = 0;
    int                              item_s = 0;
    int                              geo_s  = 0;
    crd::gpu::IRasterProgram* const  def_prog  = reinterpret_cast<crd::gpu::IRasterProgram*>(&def_s);
    crd::gpu::IRasterProgram* const  item_prog = reinterpret_cast<crd::gpu::IRasterProgram*>(&item_s);
    crd::gpu::IStorageBuffer* const  geo       = reinterpret_cast<crd::gpu::IStorageBuffer*>(&geo_s);
    FakeRTarget                      tgt(64U, 64U);

    const auto run = [&](bool patches, crd::containers::ConstSpan<RasterDrawItem> items, AmpCapEncoder& enc) {
        Context          ctx(&root);
        AmplifyBuildDesc bd;
        bd.patches        = patches;
        bd.fallback_count = 7U;
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_amplify_ceir(ctx, bd, plan));
        RenderResolvers r;
        r.target       = resolve_target;
        r.target_user  = &tgt;
        r.program      = resolve_rprog;
        r.program_user = def_prog; // the PASS DEFAULT program (a null-program item falls back to it)
        r.draws_count  = resolve_draws_count;
        r.draws_item   = resolve_draws_item;
        r.draws_user   = &items;
        REQUIRE(execute_render_lowered(ctx, ConstSpan<LoweredCommand>(plan.data(), plan.size()), enc, r)
                == ExecuteError::None);
        CHECK(enc.begins == 1); // ⭐ ONE scope for N draws (record_amplify_raster: single begin/end)
        CHECK(enc.ends == 1);
    };

    SECTION("meshlet: per-item expansion — a zero-count item is SKIPPED, a null-program item uses the pass default")
    {
        RasterDrawItem arr[3] = {
            {.program = item_prog, .storage = geo, .vertex_count = 3U}, // own program + storage
            {.program = item_prog, .vertex_count = 0U},                 // ⛔ zero count -> SKIPPED (never dispatched)
            {.vertex_count = 5U},                                       // null program -> the pass default
        };
        AmpCapEncoder enc(&root);
        run(false, crd::containers::ConstSpan<RasterDrawItem>(arr, 3U), enc);
        REQUIRE(enc.cmds.size() == 2U); // the zero-count item produced no draw
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::DispatchMesh);
        CHECK(enc.counts[0] == 3U);
        CHECK(enc.progs[0] == item_prog);
        CHECK(enc.nbinds[0] == 1U); // the per-item storage bound
        CHECK(enc.cmds[1] == crd::gpu::RasterCommandKind::DispatchMesh);
        CHECK(enc.counts[1] == 5U);
        CHECK(enc.progs[1] == def_prog); // ⭐ null-program item resolved to the pass default
        CHECK(enc.nbinds[1] == 0U);      // no storage
    }
    SECTION("patches: the primitive routes to DrawPatches + patch_count (the tess.raster half — a NEW render verb)")
    {
        RasterDrawItem arr[1] = {{.program = item_prog, .vertex_count = 4U}};
        AmpCapEncoder     enc(&root);
        run(true, crd::containers::ConstSpan<RasterDrawItem>(arr, 1U), enc);
        REQUIRE(enc.cmds.size() == 1U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::DrawPatches);
        CHECK(enc.counts[0] == 4U);
    }
    SECTION("empty DrawList -> the PROCEDURAL arm: ONE default-program draw of fallback_count")
    {
        AmpCapEncoder enc(&root);
        run(false, crd::containers::ConstSpan<RasterDrawItem>{}, enc); // empty DrawList
        REQUIRE(enc.cmds.size() == 1U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::DispatchMesh);
        CHECK(enc.counts[0] == 7U); // fallback_count
        CHECK(enc.progs[0] == def_prog);
    }
}

// CEIR-16d: a richer encoder capturing per-draw scene state — the verb (command + geometry kind), program, the run/rebase
// fields (draw_count, first_draw_index), the vertex/index count, the binding count, and the FIRST texture/sampler binding slot
// (to check atlas@4 / map@1 / sampler@5). scene_draw_list emits N verb-laddered draws in ONE scope.
struct SceneCapEncoder : crd::gpu::ICommandEncoder
{
    int                                                 begins = 0, ends = 0;
    bool                                                had_color = false; // rd.color non-empty at the last begin
    bool                                                had_depth = false; // rd.depth.enabled at the last begin
    crd::gpu::IRasterTarget*                            depth_target = nullptr; // rd.depth.target at the last begin
    crd::u32                                            rd_width = 0U, rd_height = 0U; // the resolved render area
    crd::containers::Array<crd::gpu::RasterCommandKind> cmds;
    crd::containers::Array<crd::gpu::GeometryKind>      gkinds;
    crd::containers::Array<crd::gpu::IRasterProgram*>   progs;
    crd::containers::Array<crd::u32>                    draw_counts; // multi draw_count (0 for a single verb)
    crd::containers::Array<crd::u32>                    first_rows;  // first_draw_index
    crd::containers::Array<crd::usize>                  nbinds;
    crd::containers::Array<crd::u32>                    tex_slots;   // the FIRST texture/sampler binding slot (or 0xFFFF)
    explicit SceneCapEncoder(crd::memory::IAllocator* a)
        : cmds(a), gkinds(a), progs(a), draw_counts(a), first_rows(a), nbinds(a), tex_slots(a)
    {
    }
    void begin_rendering(const crd::gpu::RenderingDesc& rd) override
    {
        ++begins;
        had_color    = rd.color.size() > 0U;
        had_depth    = rd.depth.enabled;
        depth_target = rd.depth.target;
        rd_width     = rd.width;
        rd_height    = rd.height;
    }
    void draw(const crd::gpu::RasterDrawPacket& p) override
    {
        cmds.push_back(p.command);
        gkinds.push_back(p.geometry.kind);
        progs.push_back(p.program);
        draw_counts.push_back(p.geometry.draw_count);
        first_rows.push_back(p.geometry.first_draw_index);
        nbinds.push_back(p.bindings.size());
        crd::u32 slot = 0xFFFFU;
        for (crd::usize bi = 0; bi < p.bindings.size(); ++bi)
        {
            const auto kd = p.bindings[bi].kind;
            if (kd == crd::gpu::BindingKind::SampledTexture || kd == crd::gpu::BindingKind::ComparisonSampler ||
                kd == crd::gpu::BindingKind::Sampler)
            {
                slot = p.bindings[bi].slot;
                break;
            }
        }
        tex_slots.push_back(slot);
    }
    void end_rendering() override { ++ends; }
    void dispatch(const crd::gpu::DispatchDesc&) override {}
    void transfer(const crd::gpu::TransferDesc&) override {}
    void trace_rays(const crd::gpu::TraceDesc&) override {}
};
// CEIR-16d: a pass-texture resolver over a hand-set {tex, is_depth, comparison} struct passed as `user`.
struct PassTexState
{
    crd::gpu::ITexture* tex        = nullptr;
    bool                is_depth   = false;
    bool                comparison = false;
};
crd::gpu::ITexture* resolve_pass_texture(void* user, bool& out_is_depth, bool& out_comparison)
{
    auto* const s  = static_cast<PassTexState*>(user);
    out_is_depth   = s->is_depth;
    out_comparison = s->comparison;
    return s->tex;
}
// CEIR-16d LIVE (16d-live-1a): a KIND-BRANCHING target resolver — the device-free analogue of frame_graph.cpp's fs_target.
// A color_attachment op resolves to `color`; a depth_attachment op resolves to `depth` (which may be NULL to model a forward
// pass whose colour target carries no bundled depth — the materializer must then DROP the depth attachment, not fail).
struct SceneTargetState
{
    const Context*           ctx   = nullptr;
    crd::gpu::IRasterTarget* color = nullptr;
    crd::gpu::IRasterTarget* depth = nullptr;
};
crd::gpu::IRasterTarget* resolve_scene_target(const Operation* op, void* user)
{
    auto* const s = static_cast<SceneTargetState*>(user);
    return s->ctx->op_name(op->kind()) == crd::containers::StringView("render.depth_attachment") ? s->depth : s->color;
}

TEST_CASE("ceir 16d: build_scene_ceir + execute_render_lowered expand scene_draw_list into the per-item verb ladder",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator    root;
    int                             def_s  = 0;
    int                             item_s = 0;
    int                             s0     = 0;
    int                             s1     = 0;
    int                             tx     = 0;
    int                             args_s = 0;
    crd::gpu::IRasterProgram* const def_prog = reinterpret_cast<crd::gpu::IRasterProgram*>(&def_s);
    (void)item_s;
    crd::gpu::IStorageBuffer* const buf0 = reinterpret_cast<crd::gpu::IStorageBuffer*>(&s0);
    crd::gpu::IStorageBuffer* const buf1 = reinterpret_cast<crd::gpu::IStorageBuffer*>(&s1);
    crd::gpu::IStorageBuffer* const args = reinterpret_cast<crd::gpu::IStorageBuffer*>(&args_s);
    crd::gpu::ITexture* const       atex = reinterpret_cast<crd::gpu::ITexture*>(&tx);
    FakeRTarget                     tgt(64U, 64U);

    const auto run = [&](const SceneBuildDesc& bd, crd::containers::ConstSpan<RasterDrawItem> items, PassTexState& pts,
                         SceneCapEncoder& enc) {
        Context               ctx(&root);
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_scene_ceir(ctx, bd, plan));
        RenderResolvers r;
        r.target            = resolve_target;
        r.target_user       = &tgt;
        r.program           = resolve_rprog;
        r.program_user      = def_prog; // the PASS DEFAULT program
        r.draws_count       = resolve_draws_count;
        r.draws_item        = resolve_draws_item;
        r.draws_user        = &items;
        r.pass_texture      = resolve_pass_texture;
        r.pass_texture_user = &pts;
        REQUIRE(execute_render_lowered(ctx, ConstSpan<LoweredCommand>(plan.data(), plan.size()), enc, r)
                == ExecuteError::None);
        CHECK(enc.begins == 1); // ⭐ ONE scope for the whole draw list (record_scene_raster: single begin/end)
        CHECK(enc.ends == 1);
    };

    SceneBuildDesc color_scope; // has_color=true, has_depth=false by default
    PassTexState   no_tex;

    SECTION("a plain run coalesces into ONE DrawMulti (same prog+storage), first_draw_index=0")
    {
        RasterDrawItem arr[3] = {
            {.program = def_prog, .storage = buf0, .vertex_count = 3U},
            {.program = def_prog, .storage = buf0, .vertex_count = 6U},
            {.program = def_prog, .storage = buf0, .vertex_count = 9U},
        };
        SceneCapEncoder enc(&root);
        run(color_scope, ConstSpan<RasterDrawItem>(arr, 3U), no_tex, enc);
        REQUIRE(enc.cmds.size() == 1U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::DrawMulti);
        CHECK(enc.gkinds[0] == crd::gpu::GeometryKind::MultiStoragePull);
        CHECK(enc.draw_counts[0] == 3U);
        CHECK(enc.first_rows[0] == 0U);
    }
    SECTION("a differing storage BREAKS the run -> two single plain draws")
    {
        RasterDrawItem arr[2] = {
            {.program = def_prog, .storage = buf0, .vertex_count = 3U},
            {.program = def_prog, .storage = buf1, .vertex_count = 3U},
        };
        SceneCapEncoder enc(&root);
        run(color_scope, ConstSpan<RasterDrawItem>(arr, 2U), no_tex, enc);
        REQUIRE(enc.cmds.size() == 2U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::Draw);
        CHECK(enc.cmds[1] == crd::gpu::RasterCommandKind::Draw);
    }
    SECTION("a per-item map binds at slot 1 (base-colour map) -> Draw StoragePull")
    {
        RasterDrawItem  arr[1] = {{.program = def_prog, .storage = buf0, .texture = atex, .vertex_count = 3U}};
        SceneCapEncoder enc(&root);
        run(color_scope, ConstSpan<RasterDrawItem>(arr, 1U), no_tex, enc);
        REQUIRE(enc.cmds.size() == 1U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::Draw);
        CHECK(enc.tex_slots[0] == 1U); // scene_bind_map slot 1
    }
    SECTION("a DEPTH pass atlas (no per-item map) binds tex@4 + a comparison sampler@5")
    {
        RasterDrawItem  arr[1] = {{.program = def_prog, .storage = buf0, .vertex_count = 3U}};
        PassTexState    shadow{atex, true, true};
        SceneCapEncoder enc(&root);
        run(color_scope, ConstSpan<RasterDrawItem>(arr, 1U), shadow, enc);
        REQUIRE(enc.cmds.size() == 1U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::Draw);
        CHECK(enc.tex_slots[0] == 4U); // scene_bind_atlas tex slot 4
        CHECK(enc.nbinds[0] == 3U);    // storage + atlas tex + comparison sampler
    }
    SECTION("GPU-driven indirect (args + index_count) -> DrawIndexedIndirect, first_draw_index=i")
    {
        RasterDrawItem arr[2] = {
            {.program = def_prog, .storage = buf0, .vertex_count = 3U},
            {.program = def_prog, .storage = buf1, .args = args, .index_count = 12U},
        };
        SceneCapEncoder enc(&root);
        run(color_scope, ConstSpan<RasterDrawItem>(arr, 2U), no_tex, enc);
        REQUIRE(enc.cmds.size() == 2U);
        CHECK(enc.cmds[1] == crd::gpu::RasterCommandKind::DrawIndexedIndirect);
        CHECK(enc.first_rows[1] == 1U);
    }
    SECTION("a 0-COLOUR depth-only scope is accepted (blocker #2) + items render PLAIN (a depth-only pass binds NO textures)")
    {
        SceneBuildDesc depth_only;
        depth_only.has_color     = false;
        depth_only.has_depth     = true;
        depth_only.depth_compare = crd::gpu::DepthCompare::GreaterEqual; // reverse-Z
        RasterDrawItem  arr[1] = {{.program = def_prog, .storage = buf0, .texture = atex, .vertex_count = 3U}};
        PassTexState    shadow{atex, true, true};
        SceneCapEncoder enc(&root);
        run(depth_only, ConstSpan<RasterDrawItem>(arr, 1U), shadow, enc);
        CHECK_FALSE(enc.had_color); // the scope has NO colour attachment
        REQUIRE(enc.cmds.size() == 1U);
        CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::Draw);
        CHECK(enc.nbinds[0] == 1U);           // ONLY the storage bind — scope_has_color=false suppresses ALL textures
        CHECK(enc.tex_slots[0] == 0xFFFFU);
    }
    SECTION("16z-2 PROCEDURAL mode (the visbuffer form): items render GeometryKind::None, ZERO bindings, no coalescing")
    {
        // A visbuffer (dissolved onto scene.raster, §41) draws gl_VertexIndex geometry — GeometryKind::None, vertex_count only,
        // NO storage binding. ⛔ the procedural skip is on vertex_count==0 (NOT storage): items[0]/[1] have NO storage yet MUST
        // render (the storage-null skip is the storage-ladder's RESOLVE-FAILURE guard, a DIFFERENT concern). nbinds==0
        // discriminates the procedural ladder from the storage ladder (which binds the per-item storage buffer).
        SceneBuildDesc proc; // has_color=true default
        proc.procedural = true;
        RasterDrawItem arr[3] = {
            {.program = def_prog, .vertex_count = 3U}, // no storage — procedural
            {.program = nullptr, .vertex_count = 6U},  // null program -> the pass default; still no storage
            {.program = def_prog, .vertex_count = 0U}, // vertex_count 0 -> SKIP
        };
        SceneCapEncoder enc(&root);
        run(proc, ConstSpan<RasterDrawItem>(arr, 3U), no_tex, enc);
        REQUIRE(enc.cmds.size() == 2U); // ⭐ the zero-vertex item skipped; NO coalescing (2 separate procedural draws)
        for (crd::usize i = 0; i < 2U; ++i)
        {
            CHECK(enc.cmds[i] == crd::gpu::RasterCommandKind::Draw);
            CHECK(enc.gkinds[i] == crd::gpu::GeometryKind::None); // ⭐ procedural VS (gl_VertexIndex)
            CHECK(enc.nbinds[i] == 0U);                           // ⭐ ZERO bindings (no storage pull) — the discriminator
            CHECK(enc.draw_counts[i] == 0U);                      // never coalesced
            CHECK(enc.tex_slots[i] == 0xFFFFU);                   // no textures
        }
        CHECK(enc.progs[0] == def_prog);
        CHECK(enc.progs[1] == def_prog); // the null-program item resolved to the pass default
    }
}

TEST_CASE("ceir 16d-live-4a-3: the MRT scene list expands to a SCOPE PER ITEM (begin/draw/end each, no coalesce)",
          "[ceir][ceir-gpu][render]")
{
    // The ≥2-colour scene list is the DEFERRED MRT expansion (a deferred G-buffer / WBOIT accumulate): execute_render_lowered
    // DEFERS the scope's begin_rendering, and emit_scene_list_mrt opens a SCOPE PER ITEM (begin/StoragePull-draw/end each) —
    // the legacy record_scene_raster MRT arm. ⛔ TWO same-storage plain items that COALESCE into ONE DrawMulti on the
    // single-colour path stay TWO separate per-item draws here (MRT = no coalescing, no textures). This is the shape 4b's
    // converted run_mrt_blend_gpu pixel-proves on device.
    crd::memory::GrowableTlsfAllocator    root;
    int                             def_s = 0;
    int                             s0    = 0;
    crd::gpu::IRasterProgram* const def_prog = reinterpret_cast<crd::gpu::IRasterProgram*>(&def_s);
    crd::gpu::IStorageBuffer* const buf0     = reinterpret_cast<crd::gpu::IStorageBuffer*>(&s0);
    FakeRTarget                     tgt(64U, 64U);
    RasterDrawItem                  arr[2] = {{.program = def_prog, .storage = buf0, .vertex_count = 3U},
                                             {.program = def_prog, .storage = buf0, .vertex_count = 6U}};
    ConstSpan<RasterDrawItem>       items(arr, 2U);
    PassTexState                    no_tex;

    Context               ctx(&root);
    Array<LoweredCommand> plan(&root);
    SceneBuildDesc        bd;
    bd.mrt_n    = 2U;
    bd.blend[0] = crd::gpu::BlendMode::Additive;
    bd.blend[1] = crd::gpu::BlendMode::RevealageMultiply;
    REQUIRE(build_scene_ceir(ctx, bd, plan));
    RenderResolvers r;
    r.target            = resolve_target;
    r.target_user       = &tgt;
    r.program           = resolve_rprog;
    r.program_user      = def_prog;
    r.draws_count       = resolve_draws_count;
    r.draws_item        = resolve_draws_item;
    r.draws_user        = &items;
    r.pass_texture      = resolve_pass_texture;
    r.pass_texture_user = &no_tex;
    SceneCapEncoder enc(&root);
    REQUIRE(execute_render_lowered(ctx, ConstSpan<LoweredCommand>(plan.data(), plan.size()), enc, r) == ExecuteError::None);
    CHECK(enc.begins == 2);          // ⭐ ONE scope PER ITEM (not one scope for the whole list)
    CHECK(enc.ends == 2);
    REQUIRE(enc.cmds.size() == 2U);  // ⭐ TWO draws — NO coalescing (the single-colour path would emit ONE DrawMulti)
    CHECK(enc.cmds[0] == crd::gpu::RasterCommandKind::Draw);
    CHECK(enc.cmds[1] == crd::gpu::RasterCommandKind::Draw);
    CHECK(enc.gkinds[0] == crd::gpu::GeometryKind::StoragePull);
    CHECK(enc.nbinds[0] == 1U);      // only the storage bind (a G-buffer/WBOIT item binds no textures)
    CHECK(enc.had_color);            // a 2-colour scope
}

TEST_CASE("ceir 16d-live-1a: materialize GATES the depth attachment (null-drop, extent fallback, zero-attach fail)",
          "[ceir][ceir-gpu][render]")
{
    // The depth attachment build_scene_ceir emits is a TEMPLATE the record-time target resolver GATES. This proves the
    // materializer's three record-parity behaviours device-free (fs_target's own 3-mode logic is tested in render-graph):
    //   (1) a depth op that resolves to NULL (a forward pass whose colour target carries no bundled depth) DROPS the depth
    //       attachment — depth stays disabled — matching legacy record_scene_raster's runtime-dynamic `color->has_depth()`;
    //   (2) extent_from_target falls back to the DEPTH target for a 0-colour depth-only scope (legacy `dims = color ?: depth`);
    //   (3) a scope with ZERO resolved attachments FAILS materialize (never begin_rendering an empty scope).
    crd::memory::GrowableTlsfAllocator    root;
    int                             def_s = 0;
    int                             s0    = 0;
    crd::gpu::IRasterProgram* const def_prog = reinterpret_cast<crd::gpu::IRasterProgram*>(&def_s);
    crd::gpu::IStorageBuffer* const buf0     = reinterpret_cast<crd::gpu::IStorageBuffer*>(&s0);
    FakeRTarget                     color_tgt(100U, 50U); // distinct sizes prove WHICH target the extent came from
    FakeRTarget                     depth_tgt(200U, 80U);
    RasterDrawItem                  arr[1] = {{.program = def_prog, .storage = buf0, .vertex_count = 3U}};
    ConstSpan<RasterDrawItem>       arr_span(arr, 1U); // resolve_draws_* cast void* -> ConstSpan* (non-const)
    PassTexState                    no_tex;

    const auto run = [&](const SceneBuildDesc& bd, crd::gpu::IRasterTarget* color, crd::gpu::IRasterTarget* depth,
                         SceneCapEncoder& enc) -> ExecuteError {
        Context               ctx(&root);
        Array<LoweredCommand> plan(&root);
        REQUIRE(build_scene_ceir(ctx, bd, plan));
        SceneTargetState st{&ctx, color, depth};
        RenderResolvers  r;
        r.target            = resolve_scene_target;
        r.target_user       = &st;
        r.program           = resolve_rprog;
        r.program_user      = def_prog;
        r.draws_count       = resolve_draws_count;
        r.draws_item        = resolve_draws_item;
        r.draws_user        = &arr_span;
        r.pass_texture      = resolve_pass_texture;
        r.pass_texture_user = &no_tex;
        return execute_render_lowered(ctx, ConstSpan<LoweredCommand>(plan.data(), plan.size()), enc, r);
    };

    SECTION("forward + bundled depth: depth ENABLED (target == the resolved depth), extent from the COLOUR target")
    {
        SceneBuildDesc bd; // has_color=true, has_depth=false default
        bd.has_depth = true;
        SceneCapEncoder enc(&root);
        REQUIRE(run(bd, &color_tgt, &depth_tgt, enc) == ExecuteError::None);
        CHECK(enc.begins == 1);
        CHECK(enc.had_color);
        CHECK(enc.had_depth);
        CHECK(enc.depth_target == static_cast<crd::gpu::IRasterTarget*>(&depth_tgt));
        CHECK(enc.rd_width == 100U); // colour target, NOT depth
        CHECK(enc.rd_height == 50U);
    }
    SECTION("forward + NO bundled depth (depth resolves NULL): depth DROPPED, colour still renders")
    {
        SceneBuildDesc bd;
        bd.has_depth = true;
        SceneCapEncoder enc(&root);
        REQUIRE(run(bd, &color_tgt, nullptr, enc) == ExecuteError::None); // NOT an error — the null depth is dropped
        CHECK(enc.begins == 1);
        CHECK(enc.had_color);
        CHECK_FALSE(enc.had_depth); // ⭐ the key: a null depth resolve leaves depth DISABLED, matching legacy
        CHECK(enc.rd_width == 100U);
    }
    SECTION("depth-only (0-colour) scope: extent falls back to the DEPTH target, not the 1x1 placeholder")
    {
        SceneBuildDesc bd;
        bd.has_color = false;
        bd.has_depth = true;
        SceneCapEncoder enc(&root);
        REQUIRE(run(bd, nullptr, &depth_tgt, enc) == ExecuteError::None);
        CHECK(enc.begins == 1);
        CHECK_FALSE(enc.had_color);
        CHECK(enc.had_depth);
        CHECK(enc.rd_width == 200U); // ⭐ the DEPTH target's size (legacy `dims = color ?: depth`), never the 1x1 attr
        CHECK(enc.rd_height == 80U);
    }
    SECTION("zero resolved attachments (depth-only whose depth resolves NULL): materialize FAILS, no empty begin")
    {
        SceneBuildDesc bd;
        bd.has_color = false;
        bd.has_depth = true;
        SceneCapEncoder enc(&root);
        CHECK(run(bd, nullptr, nullptr, enc) == ExecuteError::UnsupportedCommand); // never begin_rendering an empty scope
        CHECK(enc.begins == 0);
    }
}

TEST_CASE("ceir 14z-2: execute_render_lowered walks BeginRender/Draw/EndRender into the encoder (device-free FakeEncoder)",
          "[ceir][ceir-gpu][render]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    int                          tgt_s  = 0;
    int                          prog_s = 0;
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 img = declimg(ctx, k, b);
    Value* const                 col = coloratt(ctx, k, b, img);
    Value*                       atts[1] = {col};
    Operation* const             sc  = scope_op(ctx, k, b, atts, 1U, 640, 480);
    Block* const                 rb  = scope_body(ctx, sc);
    Value* const                 vc  = konst(ctx, k, rb, 3);
    Value* const                 ic  = konst(ctx, k, rb, 1);
    (void)draw_op(ctx, k, rb, vc, ic, nullptr, 0U, "", "prog");
    (void)draw_op(ctx, k, rb, vc, ic, nullptr, 0U, "", "prog");
    Array<LoweredCommand> cmds(&root);
    lower_region(ctx, *b, cmds); // [BeginRender, Draw, Draw, EndRender]
    const ConstSpan<LoweredCommand> clist(cmds.data(), cmds.size());

    SECTION("happy path — begin once, draw twice, end once, captured descs")
    {
        FakeEncoder enc;
        CHECK(execute_render_lowered(ctx, clist, enc,
                                     RenderResolvers{.target = resolve_target, .target_user = &tgt_s,
                                                     .program = resolve_rprog, .program_user = &prog_s})
              == ExecuteError::None);
        CHECK(enc.begins == 1);
        CHECK(enc.draws == 2);
        CHECK(enc.ends == 1);
        REQUIRE(enc.last_rd.color.size() == 1U); // the scope's one color attachment materialized
        CHECK(enc.last_rd.width == 640U);
        CHECK(enc.last_rd.height == 480U);
        CHECK(enc.last_draw.command == crd::gpu::RasterCommandKind::Draw); // the last Draw materialized
        CHECK(enc.last_draw.program == resolve_rprog(nullptr, &prog_s)); // == what the resolver returns (no test-side cast)
    }
    SECTION("a null program resolver -> UnresolvedProgram (typed)")
    {
        FakeEncoder enc;
        CHECK(execute_render_lowered(ctx, clist, enc,
                                     RenderResolvers{.target = resolve_target, .target_user = &tgt_s,
                                                     .program = resolve_rprog, .program_user = nullptr})
              == ExecuteError::UnresolvedProgram);
        CHECK(enc.begins == 1); // the scope began (target resolved) before the draw's null program was hit
        CHECK(enc.draws == 0);
    }
}

// ── CEIR-17b: the scene.resolve_* chain evaluator (device-free, SENTINEL callbacks) ──────────────────────────────────
namespace
{
// SENTINEL resolvers — each ENCODES its inputs into the result so the chain THREADING is verifiable (UNEQUAL derivation
// per stage: a pass-through bug that fed the wrong upstream handle yields a different number). No user state needed.
SceneResolveHandle sen_material(void* /*user*/, SceneResolveHandle draw) { return draw * 10U + 1U; }
SceneResolveHandle sen_technique(void* /*user*/, SceneResolveHandle material, StringView phase)
{
    return material * 10U + 2U
           + (phase.size() > 0U ? static_cast<SceneResolveHandle>(static_cast<unsigned char>(phase[0])) : 0U);
}
SceneResolveHandle sen_program(void* /*user*/, SceneResolveHandle technique, SceneResolveHandle draw)
{
    return technique * 100U + draw * 10U + 3U;
}
SceneResolveHandle sen_geometry(void* /*user*/, SceneResolveHandle draw) { return draw * 10U + 4U; }

// a main func body block (the resolve chain lives here — find_scene_misuse / the evaluator recurse into the func region).
Block* scene_mkmain(Context& ctx, Module& m)
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
// a seed VALUE of scene type `t` (a resource.declare with the Extern result type — the mkimage pattern).
Value* scene_seed(Context& ctx, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
} // namespace

TEST_CASE("ceir 17b: evaluate_scene_resolve THREADS a resolve chain through the host callbacks", "[ceir][ceir-gpu][scene]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)scene::register_dialect(ctx);
    Module* const m = ctx.create_module();
    Block* const  b = scene_mkmain(ctx, *m);

    Value* const     draw = scene_seed(ctx, b, scene::type_draw(ctx));
    Operation* const mat  = scene::build_resolve_material(ctx, draw, scene::type_material(ctx));
    b->append(mat);
    Operation* const tech = scene::build_resolve_technique(ctx, mat->result(0U), ctx.attr_string(StringView("opaque")),
                                                           scene::type_technique(ctx));
    b->append(tech);
    Operation* const prog = scene::build_resolve_program(ctx, tech->result(0U), draw, scene::type_program(ctx));
    b->append(prog);
    Operation* const geo = scene::build_resolve_geometry(ctx, draw, scene::type_geometry(ctx));
    b->append(geo);

    RenderResolvers r;
    r.resolve_material  = &sen_material;
    r.resolve_technique = &sen_technique;
    r.resolve_program   = &sen_program;
    r.resolve_geometry  = &sen_geometry;

    SceneResolvedHandles     out;
    const SceneResolveHandle dh = 7U;
    REQUIRE(evaluate_scene_resolve(ctx, *m, r, draw, dh, out) == ExecuteError::None);

    const SceneResolveHandle exp_mat  = dh * 10U + 1U;                                         // material from draw
    const SceneResolveHandle exp_tech = exp_mat * 10U + 2U + static_cast<SceneResolveHandle>('o'); // technique from mat + 'o'
    const SceneResolveHandle exp_prog = exp_tech * 100U + dh * 10U + 3U;                       // program from technique + draw
    const SceneResolveHandle exp_geo  = dh * 10U + 4U;                                         // geometry from draw
    CHECK(out.material == exp_mat);
    CHECK(out.technique == exp_tech); // threading: technique received EXACTLY material's output, not the draw
    CHECK(out.program == exp_prog);   // program received technique AND draw — both upstream handles
    CHECK(out.geometry == exp_geo);

    // per-PHASE distinct: a `shadow` technique resolves a DIFFERENT handle than `opaque` (the phase attr reaches the
    // callback — the evaluator read it from the op, not the caller).
    Context ctx2(&root);
    (void)func::register_dialect(ctx2);
    (void)resource::register_resource_ops(ctx2);
    (void)scene::register_dialect(ctx2);
    Module* const    m2       = ctx2.create_module();
    Block* const     b2       = scene_mkmain(ctx2, *m2);
    Value* const     seed_mat = scene_seed(ctx2, b2, scene::type_material(ctx2)); // seed a material directly
    Operation* const t2       = scene::build_resolve_technique(ctx2, seed_mat, ctx2.attr_string(StringView("shadow")),
                                                         scene::type_technique(ctx2));
    b2->append(t2);
    SceneResolvedHandles out2;
    REQUIRE(evaluate_scene_resolve(ctx2, *m2, r, seed_mat, exp_mat, out2) == ExecuteError::None);
    CHECK(out2.technique != exp_tech); // 'shadow' != 'opaque'
    CHECK(out2.technique == exp_mat * 10U + 2U + static_cast<SceneResolveHandle>('s'));
}

TEST_CASE("ceir 17b: evaluate_scene_resolve REFUSES a mistyped chain + an unwired/0 callback", "[ceir][ceir-gpu][scene]")
{
    crd::memory::GrowableTlsfAllocator root;
    RenderResolvers              all;
    all.resolve_material  = &sen_material;
    all.resolve_technique = &sen_technique;
    all.resolve_program   = &sen_program;
    all.resolve_geometry  = &sen_geometry;

    // SceneChainMisuse: resolve_technique fed a DRAW (not a material) — find_scene_misuse rejects BEFORE any callback runs.
    {
        Context ctx(&root);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)scene::register_dialect(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     b    = scene_mkmain(ctx, *m);
        Value* const     draw = scene_seed(ctx, b, scene::type_draw(ctx));
        Operation* const t    = scene::build_resolve_technique(ctx, draw, ctx.attr_string(StringView("opaque")),
                                                            scene::type_technique(ctx));
        b->append(t);
        SceneResolvedHandles out;
        CHECK(evaluate_scene_resolve(ctx, *m, all, draw, 7U, out) == ExecuteError::SceneChainMisuse);
        CHECK(out.technique == 0U); // nothing resolved — the verifier gate refused
    }
    // UnresolvedSceneHandle: a well-typed chain but the resolve_material callback is NULL (an unwired seam).
    {
        Context ctx(&root);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)scene::register_dialect(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     b    = scene_mkmain(ctx, *m);
        Value* const     draw = scene_seed(ctx, b, scene::type_draw(ctx));
        Operation* const mat  = scene::build_resolve_material(ctx, draw, scene::type_material(ctx));
        b->append(mat);
        RenderResolvers      none; // resolve_material == nullptr
        SceneResolvedHandles out;
        CHECK(evaluate_scene_resolve(ctx, *m, none, draw, 7U, out) == ExecuteError::UnresolvedSceneHandle);
    }
    // UnresolvedSceneHandle: a callback that RETURNS 0 (an unresolvable handle) is an error, not a silent 0-bind.
    {
        Context ctx(&root);
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)scene::register_dialect(ctx);
        Module* const    m    = ctx.create_module();
        Block* const     b    = scene_mkmain(ctx, *m);
        Value* const     draw = scene_seed(ctx, b, scene::type_draw(ctx));
        Operation* const mat  = scene::build_resolve_material(ctx, draw, scene::type_material(ctx));
        b->append(mat);
        RenderResolvers r;
        r.resolve_material = [](void*, SceneResolveHandle) -> SceneResolveHandle { return 0U; };
        SceneResolvedHandles out;
        CHECK(evaluate_scene_resolve(ctx, *m, r, draw, 7U, out) == ExecuteError::UnresolvedSceneHandle);
    }
}

// ── CEIR-17c: the whole-DrawList phase-discriminating parity ORACLE (device-free — the oracle is pointer-opaque) ──────
// ⛔ ADVISOR DOUBLE-RETRACTION (2026-08-14): the earlier "real scene's resolved DrawList + a scene-render DEVICE test"
// prescription is DROPPED against two pieces of evidence — (a) UNIFORMITY: fill() passes the program twins through verbatim
// (scene_renderer.cpp:5726-5727) and the C++ selection is the uniform 3-line branch at frame_runtime.cpp:192-193, so real
// items vary twin VALUES, never selection LOGIC (SceneHost is .cpp-file-local — a test cannot even name it); (b) POINTER
// OPACITY: neither path dereferences a program (add_draws_scene null-tests + copies pointers, these callbacks map
// pointer↔u64 through a host table, the assertion is pointer EQUALITY), so distinct sentinel IRasterProgram* serve
// identically to real device programs. ⇒ DEVICE-FREE, exhaustive over phases × fallback arms. The GROUND TRUTH is the
// DrawItem twin FIELDS (192-193), asserted INDEPENDENTLY of the callback's selection; distinct-per-twin sentinels + BOTH
// fallback arms catch any one-sided transcription error (the residual double-transcription of 3 cited lines is the accepted
// floor). resolve_geometry has a REAL ground truth for free (storage is attested by the C++ path — ad.storage).
namespace
{
struct C17Item
{
    crd::gpu::IRasterProgram* program          = nullptr;
    crd::gpu::IRasterProgram* program_depth     = nullptr; // null ⇒ the 192-193 fallback to `program`
    crd::gpu::IRasterProgram* program_velocity  = nullptr; // null ⇒ the 192-193 fallback to `program`
    crd::gpu::IStorageBuffer* storage           = nullptr;
};
// the INDEPENDENT ground truth — add_draws_scene's per-phase twin selection (frame_runtime.cpp:192-193), NOT the callback's
// copy: depth_only(depth/shadow) → program_depth?:program; mrt(velocity) → program_velocity?:program; else → program.
crd::gpu::IRasterProgram* c17_expected(const C17Item& it, StringView phase)
{
    if (phase == StringView("depth") || phase == StringView("shadow"))
    {
        return it.program_depth != nullptr ? it.program_depth : it.program;
    }
    if (phase == StringView("velocity")) { return it.program_velocity != nullptr ? it.program_velocity : it.program; }
    return it.program; // opaque / transparent / forward
}
// the host resolve tables (the 17c callbacks' backing). draw_handle = item index+1 (0=null). program→u64 + storage→u64
// tables (index+1) — the HOST owns the mapping; the test decodes back to the pointer (nothing smuggled through CEIR).
struct C17Host
{
    const C17Item*                                    items  = nullptr;
    crd::u32                                          count  = 0U;
    crd::containers::Array<crd::gpu::IRasterProgram*>* progs  = nullptr;
    crd::containers::Array<crd::gpu::IStorageBuffer*>* stores = nullptr;
    static crd::u32 phase_idx(StringView p) noexcept
    {
        if (p == StringView("shadow") || p == StringView("depth")) { return 1U; }
        if (p == StringView("velocity")) { return 2U; }
        return 0U; // opaque / transparent
    }
    SceneResolveHandle prog_handle(crd::gpu::IRasterProgram* p)
    {
        for (crd::u32 i = 0; i < progs->size(); ++i)
        {
            if ((*progs)[i] == p) { return i + 1U; }
        }
        progs->push_back(p);
        return static_cast<SceneResolveHandle>(progs->size());
    }
    SceneResolveHandle store_handle(crd::gpu::IStorageBuffer* s)
    {
        for (crd::u32 i = 0; i < stores->size(); ++i)
        {
            if ((*stores)[i] == s) { return i + 1U; }
        }
        stores->push_back(s);
        return static_cast<SceneResolveHandle>(stores->size());
    }
    [[nodiscard]] crd::gpu::IRasterProgram* prog_of(SceneResolveHandle h) const noexcept
    {
        return (h >= 1U && h <= progs->size()) ? (*progs)[static_cast<crd::u32>(h) - 1U] : nullptr;
    }
    [[nodiscard]] crd::gpu::IStorageBuffer* store_of(SceneResolveHandle h) const noexcept
    {
        return (h >= 1U && h <= stores->size()) ? (*stores)[static_cast<crd::u32>(h) - 1U] : nullptr;
    }
};
SceneResolveHandle c17_material(void* /*u*/, SceneResolveHandle draw) { return draw; } // material identity = the draw
SceneResolveHandle c17_technique(void* /*u*/, SceneResolveHandle material, StringView phase)
{
    return material * 8U + C17Host::phase_idx(phase); // encode (draw, phase) — a distinct id per (draw, phase)
}
SceneResolveHandle c17_program(void* u, SceneResolveHandle technique, SceneResolveHandle draw)
{
    auto* const h = static_cast<C17Host*>(u);
    if (draw < 1U || draw > h->count) { return 0U; }
    const C17Item& it   = h->items[static_cast<crd::u32>(draw) - 1U];
    const crd::u32 pidx = static_cast<crd::u32>(technique % 8U);
    crd::gpu::IRasterProgram* twin = it.program;
    if (pidx == 1U) { twin = it.program_depth != nullptr ? it.program_depth : it.program; }
    else if (pidx == 2U) { twin = it.program_velocity != nullptr ? it.program_velocity : it.program; }
    return h->prog_handle(twin);
}
SceneResolveHandle c17_geometry(void* u, SceneResolveHandle draw)
{
    auto* const h = static_cast<C17Host*>(u);
    if (draw < 1U || draw > h->count) { return 0U; }
    return h->store_handle(h->items[static_cast<crd::u32>(draw) - 1U].storage);
}
} // namespace

TEST_CASE("ceir 17c: the CEIR resolve chain reproduces add_draws_scene's per-phase twin selection (parity oracle)",
          "[ceir][ceir-gpu][scene]")
{
    crd::memory::GrowableTlsfAllocator root;
    // distinct sentinel programs / buffers (pointer-opaque — NEVER dereferenced; the reinterpret-cast-of-locals pattern).
    int        ps[8]  = {0};
    int        bs[4]  = {0};
    const auto pr     = [&](int i) { return reinterpret_cast<crd::gpu::IRasterProgram*>(&ps[i]); };
    const auto bf     = [&](int i) { return reinterpret_cast<crd::gpu::IStorageBuffer*>(&bs[i]); };

    // items EXHAUST the fallback space × distinct twins:
    C17Item items[4];
    items[0] = {pr(0), pr(1), pr(2), bf(0)};    // both twins set
    items[1] = {pr(3), nullptr, pr(4), bf(1)};  // program_depth NULL → depth/shadow FALL BACK to program
    items[2] = {pr(5), pr(6), nullptr, bf(2)};  // program_velocity NULL → velocity FALLS BACK to program
    items[3] = {pr(7), nullptr, nullptr, bf(3)}; // both NULL → EVERY phase resolves to program

    crd::containers::Array<crd::gpu::IRasterProgram*> progs(&root);
    crd::containers::Array<crd::gpu::IStorageBuffer*> stores(&root);
    C17Host                                          host;
    host.items  = items;
    host.count  = 4U;
    host.progs  = &progs;
    host.stores = &stores;

    RenderResolvers r;
    r.resolve_material      = &c17_material;
    r.resolve_technique     = &c17_technique;
    r.resolve_program       = &c17_program;
    r.resolve_program_user  = &host;
    r.resolve_geometry      = &c17_geometry;
    r.resolve_geometry_user = &host;

    const char* const phases[] = {"opaque", "transparent", "shadow", "depth", "velocity"};
    crd::u32          checked   = 0U;
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        for (const char* ph : phases)
        {
            Context ctx(&root);
            (void)func::register_dialect(ctx);
            (void)resource::register_resource_ops(ctx);
            (void)scene::register_dialect(ctx);
            Module* const    m    = ctx.create_module();
            Block* const     b    = scene_mkmain(ctx, *m);
            Value* const     draw = scene_seed(ctx, b, scene::type_draw(ctx));
            Operation* const mat  = scene::build_resolve_material(ctx, draw, scene::type_material(ctx));
            b->append(mat);
            Operation* const tech = scene::build_resolve_technique(ctx, mat->result(0U), ctx.attr_string(StringView(ph)),
                                                                   scene::type_technique(ctx));
            b->append(tech);
            Operation* const prog = scene::build_resolve_program(ctx, tech->result(0U), draw, scene::type_program(ctx));
            b->append(prog);
            Operation* const geo = scene::build_resolve_geometry(ctx, draw, scene::type_geometry(ctx));
            b->append(geo);

            SceneResolvedHandles out;
            REQUIRE(evaluate_scene_resolve(ctx, *m, r, draw, static_cast<SceneResolveHandle>(i + 1U), out)
                    == ExecuteError::None);
            // ⭐ PROGRAM parity vs the INDEPENDENT ground truth (frame_runtime.cpp:192-193 twin selection).
            CHECK(host.prog_of(out.program) == c17_expected(items[i], StringView(ph)));
            // ⭐ GEOMETRY parity — storage is attested by the C++ path (ad.storage).
            CHECK(host.store_of(out.geometry) == items[i].storage);
            ++checked;
        }
    }
    CHECK(checked == 20U); // 4 items × 5 phases — EVERY item, per phase (no silent sampling)
}
