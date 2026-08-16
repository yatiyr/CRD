// CEIR-13d parts 1b+2 — the crd-ceir-gpu lowering: lower_region → an inspectable command list (dispatches + transfers +
// interleaved barriers). ⛔ CEIR-native + validate-at-execute. Barriers derive from PRECISE per-command accesses (a
// dispatch's bindings + `access`; a transfer's static effects) — the 13a ambient is NARROWED in the bridge (the core op +
// ops_hazard stay conservative). The upload→first-read barrier is BY CONSTRUCTION and now DISCRIMINATING (present iff the
// dispatch binds the uploaded buffer). ⛔ conservative pins: malformed access → ambient; unregistered op → ambient. ⭐ part 3
// CLOSED the 12c VIEW HOLE: `gather` view-normalizes via `resource_root`, so bind view(%buf) vs %buf DOES barrier.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/transfer_ops.hpp>
#include <crd/ceir/gpu/execute.hpp> // CEIR-19c: validate_lowered — the compute surface REJECTS ceir.rt kinds
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/rt.hpp>          // CEIR-19c: the rt dialect builders (blas/instance/tlas build + ray_query)

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;      // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;

namespace
{
struct Kit
{
    OpId cst, decl, view, disp, dispi, copy, upload, readback, clear, mipgen;
    explicit Kit(Context& c)
        : cst(c.intern_op("arith", "const")), decl(c.intern_op("resource", "declare")),
          view(c.intern_op("resource", "view")), disp(c.intern_op("compute", "dispatch")),
          dispi(c.intern_op("compute", "dispatch_indirect")), copy(c.intern_op("transfer", "copy")),
          upload(c.intern_op("transfer", "upload")), readback(c.intern_op("transfer", "readback")),
          clear(c.intern_op("transfer", "clear")), mipgen(c.intern_op("transfer", "mip_gen"))
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
Operation* declbuf(Context& c, const Kit& k, Block* b)
{
    Operation* const d = c.create_operation(k.decl, {}, 1U, c.type_buffer(BufferMode::Plain, c.type_f32()));
    b->append(d);
    return d;
}
Value* view_of(Context& c, const Kit& k, Block* b, Value* res)
{
    Value*           vo[3] = {res, konst(c, k, b, 0), konst(c, k, b, 16)};
    const TypeId     vty   = c.type_view(c.type_buffer(BufferMode::Plain, c.type_f32()), static_cast<crd::u32>(ViewRange::Byte));
    Operation* const v     = c.create_operation(k.view, ConstSpan<Value*>(vo, 3U), 1U, vty);
    b->append(v);
    return v->result(0U);
}
// a direct dispatch over (g,g,g) binding `bind` (or none) with `access`.
Operation* ddispatch(Context& c, const Kit& k, Block* b, Value* g, Value* bind, const char* access, const char* kernel)
{
    Operation* d = nullptr;
    if (bind != nullptr)
    {
        Value* ops[4] = {g, g, g, bind};
        d             = c.create_operation(k.disp, ConstSpan<Value*>(ops, 4U), 0U);
    }
    else
    {
        Value* ops[3] = {g, g, g};
        d             = c.create_operation(k.disp, ConstSpan<Value*>(ops, 3U), 0U);
    }
    c.set_attr(d, "kernel", c.attr_symbol(StringView(kernel)));
    c.set_attr(d, "access", c.attr_string(StringView(access)));
    b->append(d);
    return d;
}
Operation* unary_t(Context& c, Block* b, OpId kind, Value* v)
{
    Value*           ops[1] = {v};
    Operation* const op     = c.create_operation(kind, ConstSpan<Value*>(ops, 1U), 0U);
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 13d: barriers derive from precise per-binding accesses (narrowing)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    // two dispatches SHARING a buffer (A writes, B reads) -> [Dispatch, Barrier(Raw), Dispatch].
    {
        Block* const     b   = ctx.create_block(0U);
        Value* const     g   = konst(ctx, k, b, 1);
        Operation* const buf = declbuf(ctx, k, b);
        Operation* const da  = ddispatch(ctx, k, b, g, buf->result(0U), "w", "a");
        Operation* const db  = ddispatch(ctx, k, b, g, buf->result(0U), "r", "b");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 3U);
        CHECK(out[0].op == da);
        CHECK(out[1].kind == LoweredKind::Barrier);
        CHECK(out[1].hazard == HazardKind::Raw); // ⭐ now PRECISE (write-then-read on the shared buffer)
        CHECK(out[1].before == da);
        CHECK(out[1].after == db);
        CHECK(out[2].op == db);
    }
    // two DISJOINT dispatches (bind different buffers) -> NO barrier. ⭐ the narrowing's proof. Stream order is preserved by
    // construction (command_model records in list order) -- no barrier != no order.
    {
        Block* const b  = ctx.create_block(0U);
        Value* const g  = konst(ctx, k, b, 1);
        Operation* const ba = declbuf(ctx, k, b);
        Operation* const bb = declbuf(ctx, k, b);
        (void)ddispatch(ctx, k, b, g, ba->result(0U), "w", "a");
        (void)ddispatch(ctx, k, b, g, bb->result(0U), "w", "b");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 2U);
        CHECK(out[0].kind == LoweredKind::Dispatch);
        CHECK(out[1].kind == LoweredKind::Dispatch); // ⭐ no barrier between disjoint dispatches
    }
    // three dispatches all writing the same buffer -> one barrier before B, one before C (nearest source).
    {
        Block* const     b   = ctx.create_block(0U);
        Value* const     g   = konst(ctx, k, b, 1);
        Operation* const buf = declbuf(ctx, k, b);
        Operation* const da  = ddispatch(ctx, k, b, g, buf->result(0U), "w", "a");
        Operation* const db  = ddispatch(ctx, k, b, g, buf->result(0U), "w", "b");
        (void)ddispatch(ctx, k, b, g, buf->result(0U), "w", "c");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 5U); // D, Barrier, D, Barrier, D
        CHECK(out[1].before == da);
        CHECK(out[3].before == db); // nearest strongest source
    }
}

TEST_CASE("ceir 13d: transfers lower and the upload->first-read barrier is by construction (discriminating)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    // upload(buf) then dispatch BINDING buf (read) -> [Transfer(Upload), Barrier(Raw), Dispatch]. ⭐ the headline scar.
    {
        Block* const     b   = ctx.create_block(0U);
        Value* const     g   = konst(ctx, k, b, 1);
        Operation* const buf = declbuf(ctx, k, b);
        Operation* const up  = unary_t(ctx, b, k.upload, buf->result(0U));
        Operation* const d   = ddispatch(ctx, k, b, g, buf->result(0U), "r", "kern");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 3U);
        CHECK(out[0].kind == LoweredKind::Transfer);
        CHECK(out[0].transfer_kind == LoweredTransferKind::Upload);
        CHECK(out[1].kind == LoweredKind::Barrier);
        CHECK(out[1].hazard == HazardKind::Raw); // ⭐ upload Write{buf} -> dispatch read(buf) = RAW, precise
        CHECK(out[1].before == up);
        CHECK(out[1].after == d);
        CHECK(out[2].kind == LoweredKind::Dispatch);
    }
    // upload(bufA) then dispatch binding bufB -> NO barrier (the barrier is DISCRIMINATING, not ambient).
    {
        Block* const     b  = ctx.create_block(0U);
        Value* const     g  = konst(ctx, k, b, 1);
        Operation* const ba = declbuf(ctx, k, b);
        Operation* const bb = declbuf(ctx, k, b);
        (void)unary_t(ctx, b, k.upload, ba->result(0U));
        (void)ddispatch(ctx, k, b, g, bb->result(0U), "r", "kern");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 2U);
        CHECK(out[1].kind == LoweredKind::Dispatch); // no barrier -- upload(a) does not hazard a dispatch reading b
    }
    // transfer<->transfer at the lowered-list level: upload(buf) then readback(buf) -> Barrier(Raw).
    {
        Block* const     b   = ctx.create_block(0U);
        Operation* const buf = declbuf(ctx, k, b);
        (void)unary_t(ctx, b, k.upload, buf->result(0U));
        (void)unary_t(ctx, b, k.readback, buf->result(0U));
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 3U);
        CHECK(out[1].kind == LoweredKind::Barrier);
        CHECK(out[1].hazard == HazardKind::Raw); // upload Write{buf} -> readback Read{buf}
    }
    // clear resolves its fill word (§162 inspectability).
    {
        Block* const     b   = ctx.create_block(0U);
        Operation* const buf = declbuf(ctx, k, b);
        Operation* const cl  = unary_t(ctx, b, k.clear, buf->result(0U));
        ctx.set_attr(cl, "value", ctx.attr_int(7));
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 1U);
        CHECK(out[0].kind == LoweredKind::Transfer);
        CHECK(out[0].transfer_kind == LoweredTransferKind::Clear);
        CHECK(out[0].has_clear_value);
        CHECK(out[0].clear_value == 7);
    }
}

TEST_CASE("ceir 13d: conservative pins (malformed access, unregistered op) and the CLOSED 12c view alias", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    // a dispatch with a MALFORMED access (1 binding, access "") degrades to whole-Memory rw -> barriers a prior dispatch
    // even on a disjoint buffer (conservative, never precise-but-wrong).
    {
        Block* const     b  = ctx.create_block(0U);
        Value* const     g  = konst(ctx, k, b, 1);
        Operation* const ba = declbuf(ctx, k, b);
        Operation* const bb = declbuf(ctx, k, b);
        (void)ddispatch(ctx, k, b, g, ba->result(0U), "w", "a");
        (void)ddispatch(ctx, k, b, g, bb->result(0U), "", "b"); // 1 binding but 0 tokens -> ambient
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 3U);
        CHECK(out[1].kind == LoweredKind::Barrier); // ⭐ malformed -> ambient -> barriers even the disjoint prior
    }
    // an UNREGISTERED op between two disjoint dispatches -> whole-Memory rw -> it barriers them (EMPTY != UNKNOWN).
    {
        Block* const     b  = ctx.create_block(0U);
        Value* const     g  = konst(ctx, k, b, 1);
        Operation* const ba = declbuf(ctx, k, b);
        (void)ddispatch(ctx, k, b, g, ba->result(0U), "w", "a");
        Operation* const un = ctx.create_operation(ctx.intern_op("z", "unreg"), {}, 0U); // never registered
        b->append(un);
        (void)ddispatch(ctx, k, b, g, ba->result(0U), "r", "b");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        // the unregistered op is not a lowered command, but it is NOT scanned (only emitted dispatches are `earlier`); the
        // two dispatches still share `ba` -> one barrier before the second. (The unregistered-op ambient is exercised by
        // gather when such an op is a lowered kind; here it is simply skipped -- Pure-to-the-lowering.)
        REQUIRE(out.size() == 3U);
        CHECK(out[1].kind == LoweredKind::Barrier);
    }
    // ⭐ THE 12c VIEW HOLE, CLOSED (CEIR-13d part 3): dispatch binds view(%buf), upload writes %buf. `gather` NORMALIZES the
    // bound view to its buffer root (ctx.resource_root), so upload Write{buf} -> dispatch read(view(buf)) = RAW -> BARRIER.
    {
        Block* const     b   = ctx.create_block(0U);
        Value* const     g   = konst(ctx, k, b, 1);
        Operation* const buf = declbuf(ctx, k, b);
        Operation* const up  = unary_t(ctx, b, k.upload, buf->result(0U));
        Value* const     vv  = view_of(ctx, k, b, buf->result(0U));
        Operation* const d   = ddispatch(ctx, k, b, g, vv, "r", "kern");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 3U);
        CHECK(out[0].kind == LoweredKind::Transfer);
        CHECK(out[1].kind == LoweredKind::Barrier); // ⭐ the view now names its root -> the upload hazards it
        CHECK(out[1].hazard == HazardKind::Raw);
        CHECK(out[1].before == up);
        CHECK(out[1].after == d);
        CHECK(out[2].kind == LoweredKind::Dispatch);
    }
}

TEST_CASE("ceir 13d: the grid resolves all-or-nothing; indirect and dynamic never partially fill", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    // const grid -> exact groups.
    {
        Block* const b = ctx.create_block(0U);
        (void)ddispatch(ctx, k, b, konst(ctx, k, b, 2), nullptr, "", "x"); // one const 2 feeds all three grid operands
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 1U);
        CHECK_FALSE(out[0].dynamic_grid);
        CHECK(out[0].groups_x == 2U);
        CHECK(out[0].groups_z == 2U);
    }
    // zero-const grid is lowered AS-IS (ZeroDraw is validate_dispatch's execute-time rejection).
    {
        Block* const b = ctx.create_block(0U);
        (void)ddispatch(ctx, k, b, konst(ctx, k, b, 0), nullptr, "", "x");
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 1U);
        CHECK(out[0].groups_x == 0U);
        CHECK_FALSE(out[0].dynamic_grid);
    }
    // a non-const grid operand -> dynamic_grid, groups untouched. (ddispatch reuses one g for all three; make g non-const.)
    {
        Block* const     b   = ctx.create_block(0U);
        Operation* const buf = declbuf(ctx, k, b);
        (void)ddispatch(ctx, k, b, buf->result(0U), nullptr, "", "x"); // grid operands are the buffer value (non-const)
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 1U);
        CHECK(out[0].dynamic_grid);
        CHECK(out[0].groups_x == 1U);
    }
    // indirect: Indirect, NOT dynamic_grid.
    {
        Block* const     b    = ctx.create_block(0U);
        Operation* const args = declbuf(ctx, k, b);
        Value*           io[1] = {args->result(0U)};
        Operation* const di   = ctx.create_operation(k.dispi, ConstSpan<Value*>(io, 1U), 0U);
        ctx.set_attr(di, "kernel", ctx.attr_symbol("k"));
        ctx.set_attr(di, "access", ctx.attr_string(""));
        b->append(di);
        Array<LoweredCommand> out(&root);
        lower_region(ctx, *b, out);
        REQUIRE(out.size() == 1U);
        CHECK(out[0].dispatch_kind == crd::gpu::DispatchKind::Indirect);
        CHECK_FALSE(out[0].dynamic_grid);
    }
}

// ── CEIR-19c: the ceir.rt lowering (RayQuery + AccelBuild) + the widen-closed-enum consumer audit ─────────────────────
TEST_CASE("ceir 19c: lower_region emits AccelBuild + RayQuery for ceir.rt ops; the compute surface rejects them", "[ceir][ceir-gpu][rt]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    (void)rt::register_dialect(ctx); // the rt dialect (blas/instance/tlas build + ray_query) atop the Kit's arith/resource/...

    // a well-formed blas -> instance -> tlas -> ray_query chain (the INLINE path: ray_query takes a %tlas, no SBT). The grid
    // is CONST (konst = arith.const, Index-typed) so RayQuery resolves its groups; the declares/consts emit nothing.
    Block* const     b    = ctx.create_block(0U);
    Operation* const geom = declbuf(ctx, k, b);
    Operation* const blas = rt::build_blas_build(ctx, geom->result(0U), rt::type_blas(ctx));
    b->append(blas);
    Operation* const xf   = declbuf(ctx, k, b);
    Operation* const inst = rt::build_instance_populate(ctx, blas->result(0U), xf->result(0U), ctx.attr_int(1), ctx.type_i32());
    b->append(inst);
    Operation* const tlas = rt::build_tlas_build(ctx, inst->result(0U), rt::type_tlas(ctx));
    b->append(tlas);
    Value* const     gx   = konst(ctx, k, b, 8);
    Value* const     gy   = konst(ctx, k, b, 4);
    Value* const     gz   = konst(ctx, k, b, 1);
    Operation* const bind = declbuf(ctx, k, b);
    Operation* const rq   = rt::build_ray_query(ctx, gx, gy, gz, tlas->result(0U), bind->result(0U),
                                                ctx.attr_symbol(StringView("rt_witness")), ctx.attr_string(StringView("w")));
    b->append(rq);

    Array<LoweredCommand> out(&root);
    lower_region(ctx, *b, out);

    // 3 AccelBuild (blas/instance/tlas) + 1 RayQuery, PLUS the handle-chain RAW barriers: instance reads blas's written
    // handle, tlas reads instance's, ray_query reads tlas's — three real data deps → three interleaved barriers ([blas, Bar,
    // inst, Bar, tlas, Bar, rq]). ⛔ AS builds go IN the list (the §162 no-silent-drop); barriers are INERT in
    // execute_rt_lowered (each trace_dispatch is submit+wait) but the lowering faithfully emits them.
    crd::u32              n_accel = 0U;
    crd::u32              n_rq    = 0U;
    crd::u32              n_bar   = 0U;
    const LoweredCommand* rqc     = nullptr;
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(out.size()); ++i)
    {
        if (out[i].kind == LoweredKind::AccelBuild) { ++n_accel; }
        else if (out[i].kind == LoweredKind::RayQuery) { ++n_rq; rqc = &out[i]; }
        else if (out[i].kind == LoweredKind::Barrier) { ++n_bar; }
    }
    CHECK(n_accel == 3U);
    CHECK(n_bar == 3U); // the blas->instance->tlas->ray_query handle-dependency RAW chain
    REQUIRE(n_rq == 1U);
    CHECK(rqc->op == rq);
    CHECK(rqc->groups_x == 8U); // ⭐ the const grid resolved (the direct-dispatch precedent)
    CHECK(rqc->groups_y == 4U);
    CHECK(rqc->groups_z == 1U);
    CHECK_FALSE(rqc->dynamic_grid);

    // ⭐ THE AUDIT (widen-closed-enum): the compute IComputeContext executor REJECTS ceir.rt kinds TYPED — they target the RT
    // executor (execute_rt_lowered), not this surface. The FIRST command (AccelBuild) trips it; validate never dereferences
    // the (null) resolver. (render_materialize's exhaustive switch rejects them at COMPILE time via gcc -Werror=switch.)
    const ExecuteError err =
        validate_lowered(ctx, ConstSpan<LoweredCommand>(out.data(), out.size()), nullptr, nullptr, ConstSpan<ResolvedBinding>{});
    CHECK(err == ExecuteError::UnsupportedCommand);
}

namespace
{
// A device-free recorder for the RT hooks: counts builds/traces + logs their order (0 = build, 1 = trace).
struct RtRec
{
    int      builds    = 0;
    int      traces    = 0;
    crd::u64 last_tlas = 0U;
    crd::u32 gx        = 0U;
    crd::u32 gy        = 0U;
    crd::u32 gz        = 0U;
    int      order[8]  = {};
    int      norder    = 0;
};
constexpr crd::u8 kRtSentinelBytes[4] = {1U, 2U, 3U, 4U}; // a non-empty kernel-bytes span (never dereferenced device-free)
} // namespace

// ── CEIR-19c: execute_rt_lowered drives the caller HOOKS (ceir-gpu names no backend). Device-free sentinel proof — the walk
// builds the AS chain (3x) then dispatch_inline_ray_query (1x), threading the TERMINAL tlas handle, and rejects a foreign kind.
TEST_CASE("ceir 19c: execute_rt_lowered walks the RT list through the caller hooks (build x3 -> trace x1)", "[ceir][ceir-gpu][rt]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    (void)rt::register_dialect(ctx);

    Block* const     b    = ctx.create_block(0U);
    Operation* const geom = declbuf(ctx, k, b);
    Operation* const blas = rt::build_blas_build(ctx, geom->result(0U), rt::type_blas(ctx));
    b->append(blas);
    Operation* const xf   = declbuf(ctx, k, b);
    Operation* const inst = rt::build_instance_populate(ctx, blas->result(0U), xf->result(0U), ctx.attr_int(1), ctx.type_i32());
    b->append(inst);
    Operation* const tlas = rt::build_tlas_build(ctx, inst->result(0U), rt::type_tlas(ctx));
    b->append(tlas);
    Value* const     gx   = konst(ctx, k, b, 8);
    Value* const     gy   = konst(ctx, k, b, 4);
    Value* const     gz   = konst(ctx, k, b, 1);
    Operation* const bind = declbuf(ctx, k, b);
    Operation* const rq   = rt::build_ray_query(ctx, gx, gy, gz, tlas->result(0U), bind->result(0U),
                                                ctx.attr_symbol(StringView("rt_witness")), ctx.attr_string(StringView("w")));
    b->append(rq);

    Array<LoweredCommand> out(&root);
    lower_region(ctx, *b, out);

    RtRec   rec;
    RtHooks hooks;
    hooks.build_scene = [](const Operation*, void* u) -> RtSceneHandle {
        auto* r               = static_cast<RtRec*>(u);
        r->order[r->norder++] = 0;
        return static_cast<RtSceneHandle>(++r->builds); // 1,2,3 — the tlas is the LAST build (handle 3)
    };
    hooks.kernel_bytes = [](const Operation*, void*) -> ConstSpan<crd::u8> {
        return ConstSpan<crd::u8>(kRtSentinelBytes, 4U); // file-scope sentinel (a returned span must outlive the hook)
    };
    hooks.trace_dispatch = [](RtSceneHandle tlas_h, ConstSpan<crd::u8> bytes, ConstSpan<RtHostBinding> binds, crd::u32 dx,
                              crd::u32 dy, crd::u32 dz, void* u) -> bool {
        auto* r               = static_cast<RtRec*>(u);
        r->order[r->norder++] = 1;
        ++r->traces;
        r->last_tlas = tlas_h;
        r->gx        = dx;
        r->gy        = dy;
        r->gz        = dz;
        (void)bytes;
        (void)binds;
        return true;
    };
    hooks.user = &rec;

    RtHostBinding hb;
    hb.resource = ctx.resource_root(bind->result(0U)); // the ray_query's one SSBO (operand 4)
    hb.bytes    = 16U;
    ConstSpan<RtHostBinding> binds(&hb, 1U);

    const ExecuteError rerr = execute_rt_lowered(ctx, ConstSpan<LoweredCommand>(out.data(), out.size()), hooks, binds);
    CHECK(rerr == ExecuteError::None);
    CHECK(rec.builds == 3);     // blas + instance + tlas
    CHECK(rec.traces == 1);     // the one ray_query
    CHECK(rec.last_tlas == 3U); // ⭐ the ray_query bound the TERMINAL tlas handle (build #3), threaded from tlas_build's %result
    CHECK(rec.gx == 8U);
    CHECK(rec.gy == 4U);
    CHECK(rec.gz == 1U);
    REQUIRE(rec.norder == 4); // build, build, build, trace (the AS chain fully built before the dispatch)
    CHECK(rec.order[0] == 0);
    CHECK(rec.order[1] == 0);
    CHECK(rec.order[2] == 0);
    CHECK(rec.order[3] == 1);

    // ⭐ a FOREIGN kind (a compute dispatch) on the RT surface → UnsupportedCommand (the mirror of execute_lowered rejecting RT).
    Block* const cb = ctx.create_block(0U);
    (void)ddispatch(ctx, k, cb, konst(ctx, k, cb, 1), nullptr, "", "x");
    Array<LoweredCommand> cout(&root);
    lower_region(ctx, *cb, cout);
    CHECK(execute_rt_lowered(ctx, ConstSpan<LoweredCommand>(cout.data(), cout.size()), hooks, ConstSpan<RtHostBinding>{})
          == ExecuteError::UnsupportedCommand);
}

// ── CEIR-19c: the DEFERRAL is a PIN, not prose — rt.trace + rt.sbt_build lower to NOTHING (DEFERRED to 19z). ────────────
// 19c ships ray_query + AS-builds ONLY; rt.trace (the pipeline path) + rt.sbt_build are declared but not yet lowered. They
// are NOT in lower_region's recognized set, so they `continue` (emit nothing) — this test makes that a COMMITTED behavior a
// 19z implementer trips on purpose, closing the only untested branch the RT lowering added.
TEST_CASE("ceir 19c: rt.trace + rt.sbt_build are DEFERRED to 19z (they lower to no command)", "[ceir][ceir-gpu][rt]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    (void)rt::register_dialect(ctx);

    Block* const     b    = ctx.create_block(0U);
    Operation* const geom = declbuf(ctx, k, b);
    Operation* const blas = rt::build_blas_build(ctx, geom->result(0U), rt::type_blas(ctx));
    b->append(blas);
    Operation* const xf   = declbuf(ctx, k, b);
    Operation* const inst = rt::build_instance_populate(ctx, blas->result(0U), xf->result(0U), ctx.attr_int(1), ctx.type_i32());
    b->append(inst);
    Operation* const tlas = rt::build_tlas_build(ctx, inst->result(0U), rt::type_tlas(ctx));
    b->append(tlas);
    Operation* const sbt = rt::build_sbt_build(ctx, ctx.attr_symbol(StringView("rgen")), rt::type_sbt(ctx));
    b->append(sbt);
    Value* const     bind = declbuf(ctx, k, b)->result(0U);
    Operation* const tr   = rt::build_trace(ctx, konst(ctx, k, b, 1), konst(ctx, k, b, 1), konst(ctx, k, b, 1),
                                            tlas->result(0U), sbt->result(0U), bind, ctx.attr_string(StringView("w")));
    b->append(tr);

    Array<LoweredCommand> out(&root);
    lower_region(ctx, *b, out);

    // The 3 AccelBuild (blas/instance/tlas) still lower; sbt_build + trace emit NOTHING (no command carries their op).
    crd::u32 n_accel = 0U;
    crd::u32 n_rq    = 0U;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(out.size()); ++i)
    {
        if (out[i].kind == LoweredKind::AccelBuild) { ++n_accel; }
        else if (out[i].kind == LoweredKind::RayQuery) { ++n_rq; }
        CHECK(out[i].op != sbt); // ⛔ rt.sbt_build lowered to no command (DEFERRED)
        CHECK(out[i].op != tr);  // ⛔ rt.trace lowered to no command (DEFERRED)
    }
    CHECK(n_accel == 3U);
    CHECK(n_rq == 0U); // no ray_query in this chain — trace is the pipeline path, not the inline one
}

TEST_CASE("ceir 13d: lowering is deterministic (dispatches + a transfer, lower twice -> identical)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);
    Block* const                 b   = ctx.create_block(0U);
    Value* const                 g   = konst(ctx, k, b, 7);
    Operation* const             buf = declbuf(ctx, k, b);
    (void)unary_t(ctx, b, k.upload, buf->result(0U));
    (void)ddispatch(ctx, k, b, g, buf->result(0U), "r", "a");
    (void)ddispatch(ctx, k, b, g, buf->result(0U), "w", "b");

    Array<LoweredCommand> a(&root);
    Array<LoweredCommand> c(&root);
    lower_region(ctx, *b, a);
    lower_region(ctx, *b, c);
    REQUIRE(a.size() == c.size());
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        CHECK(a[i].kind == c[i].kind);
        CHECK(a[i].op == c[i].op);
        CHECK(a[i].hazard == c[i].hazard);
        CHECK(a[i].before == c[i].before);
        CHECK(a[i].transfer_kind == c[i].transfer_kind);
        CHECK(a[i].dynamic_grid == c[i].dynamic_grid);
    }
}

TEST_CASE("ceir 13d: a dispatch reading N buffers from N prior writers gets N per-resource barriers (13z-3 completion)", "[ceir][ceir-gpu]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const Kit                    k(ctx);

    // A writes bufA ; B writes bufB ; C reads BOTH bufA and bufB. ⭐ C must get TWO barriers (RAW on bufA from A, RAW on bufB
    // from B) — the old "one barrier per dispatch, strongest" DROPPED one conflict; the FFT is exactly where that is real.
    Block* const     b   = ctx.create_block(0U);
    Value* const     g   = konst(ctx, k, b, 1);
    Operation* const ba  = declbuf(ctx, k, b);
    Operation* const bb  = declbuf(ctx, k, b);
    Operation* const da  = ddispatch(ctx, k, b, g, ba->result(0U), "w", "a");
    Operation* const db  = ddispatch(ctx, k, b, g, bb->result(0U), "w", "b");
    Value*           co[5] = {g, g, g, ba->result(0U), bb->result(0U)};
    Operation* const dc  = ctx.create_operation(k.disp, ConstSpan<Value*>(co, 5U), 0U);
    ctx.set_attr(dc, "kernel", ctx.attr_symbol(StringView("c")));
    ctx.set_attr(dc, "access", ctx.attr_string(StringView("r,r")));
    b->append(dc);

    Array<LoweredCommand> out(&root);
    lower_region(ctx, *b, out);
    // D(a), D(b), Barrier(bufA), Barrier(bufB), D(c) — the two writers are disjoint (no barrier between them).
    REQUIRE(out.size() == 5U);
    CHECK(out[0].op == da);
    CHECK(out[1].op == db);
    CHECK(out[2].kind == LoweredKind::Barrier);
    CHECK(out[3].kind == LoweredKind::Barrier);
    CHECK(out[4].op == dc);
    // both barriers precede dc, are RAW, and name the two DISTINCT root resources in binding-operand order (bufA then bufB).
    CHECK(out[2].after == dc);
    CHECK(out[3].after == dc);
    CHECK(out[2].hazard == HazardKind::Raw);
    CHECK(out[3].hazard == HazardKind::Raw);
    CHECK(out[2].resource == ba->result(0U));
    CHECK(out[2].before == da);
    CHECK(out[3].resource == bb->result(0U));
    CHECK(out[3].before == db);
}
