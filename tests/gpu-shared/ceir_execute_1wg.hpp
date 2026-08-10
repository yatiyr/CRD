#pragma once

// ceir_execute_1wg.hpp — CEIR-13z-1b: the SHARED both-backend harness for running a CEIR compute ASSET on a real device
// through `crd::ceir::gpu::execute_lowered` (ADR-0126), and proving it BYTE-IDENTICAL to the direct CKIR dispatch. It clones
// `dispatch_kernel_1wg` (ckir_kernel_dispatch.hpp) EXACTLY — same buffer creation, upload copies, TransferDst→ShaderRead
// barriers, readback — replacing ONLY the `rec.dispatch(...)` line with `execute_lowered(...)`. SAME pipeline object on both
// paths; the CEIR authoring/lowering/binding indirection must not perturb a byte. ⛔ CONTRACT: the CEIR dispatch's binding-
// operand order == the KGraph `buffer_decl` binding indices 0..n-1 (the 13a positional-slot rule).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/kir/ckir.hpp>

#include <crd/gpu/compute.hpp>

#include <crd/containers/span.hpp>

#include <memory>

namespace crd::ceir_gpu_test
{
// The element-wise ADD kernel: `c[lid] = a[lid] + b[lid]` over F32 buffers. Buffers: a (0,0 read), b (0,1 read), c (0,2
// write). One workgroup of `ls` threads. The KGraph an emitter turns into GLSL/HLSL, then a ComputePipeline.
inline crd::kir::KEntry build_add_kernel(crd::kir::KGraph& g, int ls)
{
    namespace k = crd::kir;
    const int a   = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int b   = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int c   = g.buffer_decl(k::DType::F32, 0, 2, true);
    const int lid = g.builtin(k::KBuiltin::LocalInvocationIndex);

    const int mark = g.kernel_stmt_mark();
    g.stmt_buffer_store(c, lid, g.binary(k::KOp::Add, g.buffer_load(a, lid), g.buffer_load(b, lid)));

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(ls);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// A built CEIR dispatch asset: the owning MODULE (for text print/parse), its body BLOCK (for lowering), and the binding
// Values in KGraph buffer-index order. ⛔ the SAME Context must live for the lowering + execute (resource_root).
struct CeirDispatchAsset
{
    crd::ceir::Module*      module = nullptr;
    crd::ceir::Block*       block  = nullptr;
    const crd::ceir::Value* binds[8]{};
    int                     nbinds = 0;
};

// Build a CEIR compute ASSET into `c`: a MODULE whose body block is a grid const (1 workgroup) + `nbufs` resource.declare +
// one compute.dispatch(grid×3, buf0..bufN) kernel=@`kernel` access=`access` (must have `nbufs` comma tokens matching the
// KGraph buffer_decl order). Registers the dialects. ⭐ CEIR-13z-2: module-wrapped so the asset can be print/parse'd (§121
// text≡builder); the block is transparent to lower_region.
inline CeirDispatchAsset build_ceir_dispatch_asset(crd::ceir::Context& c, const char* kernel, const char* access, int nbufs)
{
    namespace ce = crd::ceir;
    (void)ce::arith::register_arith_ops(c);
    (void)ce::func::register_dialect(c);
    (void)ce::resource::register_resource_ops(c);
    (void)ce::compute::register_compute_ops(c);
    const ce::OpId cst  = c.intern_op("arith", "const");
    const ce::OpId decl = c.intern_op("resource", "declare");
    const ce::OpId disp = c.intern_op("compute", "dispatch");

    CeirDispatchAsset asset;
    asset.module = c.create_module();
    ce::Block* const b = c.create_block(0U);
    asset.module->body()->append(b);
    asset.block  = b;
    asset.nbinds = nbufs;

    ce::Operation* const g = c.create_operation(cst, {}, 1U, c.type_index());
    c.set_attr(g, "value", c.attr_int(1));
    b->append(g);
    ce::Value* const grid = g->result(0U);

    ce::Value* ops[3 + 8];
    ops[0] = grid;
    ops[1] = grid;
    ops[2] = grid;
    for (int i = 0; i < nbufs; ++i)
    {
        ce::Operation* const d = c.create_operation(decl, {}, 1U, c.type_buffer(ce::BufferMode::Plain, c.type_f32()));
        b->append(d);
        ops[3 + i]       = d->result(0U);
        asset.binds[i]   = d->result(0U);
    }
    ce::Operation* const dd = c.create_operation(disp, crd::containers::ConstSpan<ce::Value*>(ops, static_cast<crd::usize>(3 + nbufs)), 0U);
    c.set_attr(dd, "kernel", c.attr_symbol(crd::containers::StringView(kernel)));
    c.set_attr(dd, "access", c.attr_string(crd::containers::StringView(access)));
    b->append(dd);
    return asset;
}

// Back-compat (CEIR-13z-1b): the `add` asset (3 buffers a,b,c; access r,r,w) returning just the block + binds — a thin
// wrapper over the generalized (now module-wrapped) builder, so the add device tests are unchanged.
inline crd::ceir::Block* build_add_ceir_asset(crd::ceir::Context& c, const crd::ceir::Value* out_binds[3])
{
    const CeirDispatchAsset a = build_ceir_dispatch_asset(c, "add", "r,r,w", 3);
    for (int i = 0; i < 3; ++i) { out_binds[i] = a.binds[i]; }
    return a.block;
}

// The resolver: every dispatch resolves to the one pipeline handed via `user` (the direct path's pipeline — same object).
inline crd::gpu::ComputePipeline* resolve_single_pipeline(const crd::ceir::Operation*, void* user)
{
    return static_cast<crd::gpu::ComputePipeline*>(user);
}

// ── CEIR-13z-3 part 3: the MULTI-DISPATCH asset (the FFT chain) ──────────────────────────────────────────────────────────
// One pass of a multi-dispatch asset: a kernel symbol, its `access` tokens, its logical-buffer bindings (indices into the
// asset's buffer list), and its workgroup grid (X; Y=Z=1).
struct MultiPass
{
    const char* kernel;
    const char* access;
    int         bind[8];
    int         nbind;
    int         grid;
};
// A built multi-dispatch asset: the MODULE, its BLOCK, the logical-buffer Values (in declaration order), and the dispatch
// OPS in pass order (for the by-identity resolver).
struct CeirMultiAsset
{
    crd::ceir::Module*          module = nullptr;
    crd::ceir::Block*           block  = nullptr;
    const crd::ceir::Value*     buffers[20]{};
    int                         nbuffers = 0;
    const crd::ceir::Operation* dispatches[8]{};
    int                         ndispatch = 0;
};

// Build a MULTI-DISPATCH CEIR asset: `nbuffers` resource.declare + `npasses` compute.dispatch, each dispatch binding its
// pass's buffers (grid = (pass.grid, 1, 1)). ⭐ 13z-3: the lowering derives the inter-pass PER-RESOURCE barriers from the
// access tokens; execute_lowered replays them.
inline CeirMultiAsset build_ceir_multi_asset(crd::ceir::Context& c, int nbuffers, const MultiPass* passes, int npasses)
{
    namespace ce = crd::ceir;
    (void)ce::arith::register_arith_ops(c);
    (void)ce::func::register_dialect(c);
    (void)ce::resource::register_resource_ops(c);
    (void)ce::compute::register_compute_ops(c);
    const ce::OpId cst  = c.intern_op("arith", "const");
    const ce::OpId decl = c.intern_op("resource", "declare");
    const ce::OpId disp = c.intern_op("compute", "dispatch");

    CeirMultiAsset asset;
    asset.module = c.create_module();
    ce::Block* const b = c.create_block(0U);
    asset.module->body()->append(b);
    asset.block    = b;
    asset.nbuffers = nbuffers;

    ce::Value* bufv[20]{}; // non-const for create_operation operands (asset.buffers is the const view)
    for (int i = 0; i < nbuffers; ++i)
    {
        ce::Operation* const d = c.create_operation(decl, {}, 1U, c.type_buffer(ce::BufferMode::Plain, c.type_f32()));
        b->append(d);
        bufv[i]          = d->result(0U);
        asset.buffers[i] = d->result(0U);
    }
    const auto konst_i = [&](int v) -> ce::Value* {
        ce::Operation* const o = c.create_operation(cst, {}, 1U, c.type_index());
        c.set_attr(o, "value", c.attr_int(v));
        b->append(o);
        return o->result(0U);
    };
    asset.ndispatch = npasses;
    for (int pi = 0; pi < npasses; ++pi)
    {
        const MultiPass& p  = passes[pi];
        ce::Value* const gx = konst_i(p.grid); // grid = (grid, 1, 1) — the FFT passes are 1-D workgroup grids
        ce::Value* const gy = konst_i(1);
        ce::Value* const gz = konst_i(1);
        ce::Value*       ops[3 + 8];
        ops[0] = gx;
        ops[1] = gy;
        ops[2] = gz;
        for (int k = 0; k < p.nbind; ++k) { ops[3 + k] = bufv[p.bind[k]]; }
        ce::Operation* const dd = c.create_operation(disp, crd::containers::ConstSpan<ce::Value*>(ops, static_cast<crd::usize>(3 + p.nbind)), 0U);
        c.set_attr(dd, "kernel", c.attr_symbol(crd::containers::StringView(p.kernel)));
        c.set_attr(dd, "access", c.attr_string(crd::containers::StringView(p.access)));
        b->append(dd);
        asset.dispatches[pi] = dd;
    }
    return asset;
}

// The MULTI-kernel resolver: map a dispatch OP → its pipeline BY IDENTITY (dispatch order). ⛔ NO registry (the advisor's
// simplification) — a flat parallel array of {op, pipe}. Stateless (no counter to consume twice across validate/execute).
struct MultiResolve
{
    const crd::ceir::Operation* const* ops;
    crd::gpu::ComputePipeline* const*   pipes;
    int                                 n;
};
inline crd::gpu::ComputePipeline* resolve_multi(const crd::ceir::Operation* d, void* user)
{
    const auto* m = static_cast<const MultiResolve*>(user);
    for (int i = 0; i < m->n; ++i)
    {
        if (m->ops[i] == d) { return m->pipes[i]; }
    }
    return nullptr;
}

// Collect the binding operands (in order, skipping the 3 grid operands) of the single compute.dispatch in `block` — for
// executing a PARSED module (whose binding Values come from parse, not a builder). Returns the binding count.
inline int collect_dispatch_binds(const crd::ceir::Context& ctx, const crd::ceir::Block& block, const crd::ceir::Value* out[8])
{
    for (const crd::ceir::Operation* op = block.first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == crd::containers::StringView("compute.dispatch"))
        {
            const int nb = op->num_operands() >= 3U ? static_cast<int>(op->num_operands()) - 3 : 0;
            for (int i = 0; i < nb && i < 8; ++i) { out[i] = op->operand(static_cast<crd::u32>(3 + i)); }
            return nb;
        }
    }
    return 0;
}

// Run the lowered CEIR command list on `ctx` over ONE workgroup, EXACTLY as dispatch_kernel_1wg but with execute_lowered in
// place of rec.dispatch. `host[b]` (nbufs F32 arrays of length lens[b]) is uploaded then overwritten with the GPU result.
// `ceir_binds[b]` are the CEIR binding Values in buffer order; `cctx` is the asset's Context (resource_root). Returns the
// ExecuteError (None on success). No push constants (local_size baked into the kernel), so pipe was created with push 0.
inline crd::ceir::gpu::ExecuteError
dispatch_ceir_1wg(crd::ceir::Context& cctx, crd::containers::ConstSpan<crd::ceir::gpu::LoweredCommand> cmds,
                  const crd::ceir::Value* const* ceir_binds, crd::gpu::ComputePipeline& pipe,
                  crd::gpu::IComputeContext& ctx, float** host, const int* lens, int nbufs, crd::u32 gx)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    constexpr int                     max_bufs = 8;
    std::unique_ptr<g::ComputeBuffer> dev[max_bufs];
    std::unique_ptr<g::ComputeBuffer> up[max_bufs];
    std::unique_ptr<g::ComputeBuffer> rb[max_bufs];
    crd::ceir::gpu::ResolvedBinding   table[max_bufs];

    for (int b = 0; b < nbufs; ++b)
    {
        const crd::u64 bytes = static_cast<crd::u64>(lens[b]) * sizeof(float);
        dev[b] = ctx.create_buffer(bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        up[b]  = ctx.create_buffer(bytes, transfer_src, g::ComputeMemory::CpuToGpu);
        rb[b]  = ctx.create_buffer(bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        auto* p = static_cast<float*>(up[b]->map());
        for (int i = 0; i < lens[b]; ++i) { p[i] = host[b][i]; }
        up[b]->unmap();
        table[b] = {ceir_binds[b], dev[b].get()}; // CEIR Value -> the live device buffer (binding order)
    }

    auto& rec = ctx.begin();
    for (int b = 0; b < nbufs; ++b) { rec.copy(*up[b], *dev[b], 0U, 0U, static_cast<crd::u64>(lens[b]) * sizeof(float)); }
    for (int b = 0; b < nbufs; ++b) { rec.barrier(*dev[b], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }

    // ⭐ the ONE swapped line: execute_lowered records the dispatch (resolve kernel + gather bindings) instead of rec.dispatch.
    (void)gx; // the CEIR grid const (1) drives the workgroup count; gx is the caller's intent, asserted equal by the asset
    const crd::ceir::gpu::ExecuteError err =
        crd::ceir::gpu::execute_lowered(cctx, cmds, rec, resolve_single_pipeline, &pipe,
                                        crd::containers::ConstSpan<crd::ceir::gpu::ResolvedBinding>(table, static_cast<crd::usize>(nbufs)));

    for (int b = 0; b < nbufs; ++b)
    {
        rec.barrier(*dev[b], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[b], *rb[b], 0U, 0U, static_cast<crd::u64>(lens[b]) * sizeof(float));
    }
    ctx.submit_and_wait();

    for (int b = 0; b < nbufs; ++b)
    {
        const auto* r = static_cast<const float*>(rb[b]->map());
        for (int i = 0; i < lens[b]; ++i) { host[b][i] = r[i]; }
        rb[b]->unmap();
    }
    return err;
}

// ⭐ CEIR-13z-3 part 3: run a MULTI-DISPATCH CEIR asset on a device — a byte-for-byte clone of `dispatch_fft2d`
// (ckir_kernel_dispatch.hpp) with `execute_lowered` REPLACING the per-pass dispatch+barrier loop. Same persistent buffers,
// same upload copies + `TransferDst→ShaderRead` barrier (the HARNESS owns the upload barrier — the CEIR asset is dispatch-
// only), same readback. `execute_lowered` derives the INTER-PASS barriers from the lowered command list (the per-resource
// barriers land between passes) and resolves each dispatch to its pass pipeline BY IDENTITY. `sizes[b]`/`host[b]` are indexed
// by logical buffer id; `pipes[pi]` by pass id. Returns the ExecuteError.
inline crd::ceir::gpu::ExecuteError
dispatch_ceir_multi(crd::ceir::Context& cctx, const CeirMultiAsset& asset,
                    crd::containers::ConstSpan<crd::ceir::gpu::LoweredCommand> cmds, const int* sizes,
                    crd::gpu::ComputePipeline* const* pipes, crd::gpu::IComputeContext& ctx, float** host)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    constexpr int                     max_bufs = 20;
    std::unique_ptr<g::ComputeBuffer> dev[max_bufs];
    std::unique_ptr<g::ComputeBuffer> up[max_bufs];
    std::unique_ptr<g::ComputeBuffer> rb[max_bufs];
    crd::ceir::gpu::ResolvedBinding   table[max_bufs];
    const int                         nb = asset.nbuffers;

    for (int b = 0; b < nb; ++b)
    {
        const crd::u64 bytes = static_cast<crd::u64>(sizes[b]) * sizeof(float);
        dev[b] = ctx.create_buffer(bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        up[b]  = ctx.create_buffer(bytes, transfer_src, g::ComputeMemory::CpuToGpu);
        rb[b]  = ctx.create_buffer(bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        auto* p = static_cast<float*>(up[b]->map());
        for (int i = 0; i < sizes[b]; ++i) { p[i] = host[b][i]; }
        up[b]->unmap();
        table[b] = {asset.buffers[b], dev[b].get()};
    }

    MultiResolve mr{asset.dispatches, pipes, asset.ndispatch};

    auto& rec = ctx.begin();
    for (int b = 0; b < nb; ++b) { rec.copy(*up[b], *dev[b], 0U, 0U, static_cast<crd::u64>(sizes[b]) * sizeof(float)); }
    for (int b = 0; b < nb; ++b) { rec.barrier(*dev[b], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }

    // ⭐ execute_lowered records every pass dispatch + the DERIVED inter-pass barriers (replacing dispatch_fft2d's manual loop).
    const crd::ceir::gpu::ExecuteError err = crd::ceir::gpu::execute_lowered(
        cctx, cmds, rec, resolve_multi, &mr, crd::containers::ConstSpan<crd::ceir::gpu::ResolvedBinding>(table, static_cast<crd::usize>(nb)));

    for (int b = 0; b < nb; ++b)
    {
        rec.barrier(*dev[b], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[b], *rb[b], 0U, 0U, static_cast<crd::u64>(sizes[b]) * sizeof(float));
    }
    ctx.submit_and_wait();

    for (int b = 0; b < nb; ++b)
    {
        const auto* r = static_cast<const float*>(rb[b]->map());
        for (int i = 0; i < sizes[b]; ++i) { host[b][i] = r[i]; }
        rb[b]->unmap();
    }
    return err;
}
} // namespace crd::ceir_gpu_test
