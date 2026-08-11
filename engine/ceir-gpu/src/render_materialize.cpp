#include <crd/ceir/gpu/render_materialize.hpp>

#include <crd/ceir/attr.hpp>

#include <crd/containers/array.hpp> // CEIR-14z-4a: the reserved per-scope pass-closure array

#include <cstring> // std::memcpy — read a Float attr's f64 bit pattern

namespace crd::ceir::gpu
{
namespace
{
[[nodiscard]] containers::StringView str_attr(const Context& ctx, const Operation* op, containers::StringView name)
{
    const AttrId a = op->attr(name);
    if (!a.valid()) { return {}; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::String ? v.s : containers::StringView{};
}
[[nodiscard]] crd::i64 int_attr(const Context& ctx, const Operation* op, containers::StringView name, crd::i64 dflt)
{
    const AttrId a = op->attr(name);
    if (!a.valid()) { return dflt; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::Int ? v.i : dflt;
}
[[nodiscard]] float float_attr(const Context& ctx, const Operation* op, containers::StringView name, float dflt)
{
    const AttrId a = op->attr(name);
    if (!a.valid()) { return dflt; }
    const AttrValue v = ctx.attr_value(a);
    if (v.kind != AttrKind::Float) { return dflt; }
    crd::f64 d = 0.0;
    std::memcpy(&d, &v.f, sizeof(d)); // Float is stored as the f64 bit pattern
    return static_cast<float>(d);
}
// CEIR string attr → the command_model enums (absent = the enum default). ⛔ find_render_misuse has already pinned the
// closed vocabularies, so the fall-through defaults here fire only on the absent case, never on a bad token.
[[nodiscard]] crd::gpu::LoadOp load_op_of(const Context& ctx, const Operation* op)
{
    const containers::StringView s = str_attr(ctx, op, containers::StringView("load"));
    if (s == containers::StringView("load")) { return crd::gpu::LoadOp::Load; }
    if (s == containers::StringView("dontcare")) { return crd::gpu::LoadOp::DontCare; }
    return crd::gpu::LoadOp::Clear;
}
[[nodiscard]] crd::gpu::StoreOp store_op_of(const Context& ctx, const Operation* op)
{
    return str_attr(ctx, op, containers::StringView("store")) == containers::StringView("dontcare")
             ? crd::gpu::StoreOp::DontCare
             : crd::gpu::StoreOp::Store;
}
[[nodiscard]] crd::gpu::BlendMode blend_of(const Context& ctx, const Operation* op)
{
    const containers::StringView s = str_attr(ctx, op, containers::StringView("blend"));
    if (s == containers::StringView("alpha")) { return crd::gpu::BlendMode::Alpha; }
    if (s == containers::StringView("additive")) { return crd::gpu::BlendMode::Additive; }
    if (s == containers::StringView("premultiplied")) { return crd::gpu::BlendMode::PremultipliedAlpha; }
    return crd::gpu::BlendMode::Opaque;
}
[[nodiscard]] crd::gpu::DepthCompare compare_of(const Context& ctx, const Operation* op)
{
    const containers::StringView s = str_attr(ctx, op, containers::StringView("compare"));
    if (s == containers::StringView("never")) { return crd::gpu::DepthCompare::Never; }
    if (s == containers::StringView("less")) { return crd::gpu::DepthCompare::Less; }
    if (s == containers::StringView("equal")) { return crd::gpu::DepthCompare::Equal; }
    if (s == containers::StringView("greater")) { return crd::gpu::DepthCompare::Greater; }
    if (s == containers::StringView("not_equal")) { return crd::gpu::DepthCompare::NotEqual; }
    if (s == containers::StringView("greater_equal")) { return crd::gpu::DepthCompare::GreaterEqual; }
    if (s == containers::StringView("always")) { return crd::gpu::DepthCompare::Always; }
    return crd::gpu::DepthCompare::LessEqual;
}
// Resolve a count operand to a compile-time u32 (arith.const), mirroring lower.cpp::resolve_const_u32.
[[nodiscard]] bool const_u32(const Context& ctx, const Value* v, crd::u32& out)
{
    const Operation* const def = v->defining_op();
    if (def == nullptr) { return false; }
    if (ctx.op_name(def->kind()) != containers::StringView("arith.const")) { return false; }
    const AttrValue av = ctx.attr_value(def->attr(containers::StringView("value")));
    if (av.kind != AttrKind::Int) { return false; }
    out = static_cast<crd::u32>(av.i);
    return true;
}
} // namespace

bool materialize_rendering_desc(const Context& ctx, const Operation* scope_op, RasterTargetResolveFn resolver, void* user,
                                crd::gpu::RenderingDesc& out)
{
    out = crd::gpu::RenderingDesc{};
    out.width  = static_cast<crd::u32>(int_attr(ctx, scope_op, containers::StringView("width"), 0));
    out.height = static_cast<crd::u32>(int_attr(ctx, scope_op, containers::StringView("height"), 0));
    out.sample_count = static_cast<crd::u32>(int_attr(ctx, scope_op, containers::StringView("sample_count"), 1));
    for (crd::u32 i = 0; i < scope_op->num_operands(); ++i)
    {
        const Operation* const att = scope_op->operand(i)->defining_op();
        if (att == nullptr) { return false; } // an attachment operand must be a color/depth_attachment op result
        const containers::StringView nm = ctx.op_name(att->kind());
        if (nm == containers::StringView("render.color_attachment"))
        {
            if (out.color.size() >= crd::gpu::kMaxColorAttachments) { return false; }
            crd::gpu::ColorAttachmentDesc c;
            c.target = resolver(att, user);
            c.load   = load_op_of(ctx, att);
            c.store  = store_op_of(ctx, att);
            c.blend  = blend_of(ctx, att);
            // ⭐ RAH-1a.1 end-to-end: a `uint` typed-clear selects clear_uint (the R32_UINT id target) over the float clear.
            if (str_attr(ctx, att, containers::StringView("clear_kind")) == containers::StringView("uint"))
            {
                c.clear_kind = crd::gpu::ClearKind::Uint;
                c.clear_uint = static_cast<crd::u32>(int_attr(ctx, att, containers::StringView("clear_uint"), 0));
            }
            else
            {
                c.clear_kind = crd::gpu::ClearKind::Float;
                c.clear.r    = float_attr(ctx, att, containers::StringView("clear_r"), 0.0F);
                c.clear.g    = float_attr(ctx, att, containers::StringView("clear_g"), 0.0F);
                c.clear.b    = float_attr(ctx, att, containers::StringView("clear_b"), 0.0F);
                c.clear.a    = float_attr(ctx, att, containers::StringView("clear_a"), 0.0F);
            }
            out.color.push_back(c);
        }
        else if (nm == containers::StringView("render.depth_attachment"))
        {
            out.depth.enabled     = true;
            out.depth.target      = resolver(att, user);
            out.depth.load        = load_op_of(ctx, att);
            out.depth.store       = store_op_of(ctx, att);
            out.depth.clear_depth = float_attr(ctx, att, containers::StringView("clear_depth"), 1.0F);
            out.depth.compare     = compare_of(ctx, att);
            // ⛔ `read_only` → the per-draw depth-WRITE disable (RasterState), a draw-state concern; named-forward.
        }
        else
        {
            return false;
        }
    }
    return true;
}

bool materialize_draw_packet(const Context& ctx, const Operation* draw_op, RasterProgramResolveFn resolver, void* user,
                             crd::gpu::RasterDrawPacket& out, RasterBindingResolveFn binding_resolver, void* binding_user)
{
    out         = crd::gpu::RasterDrawPacket{};
    out.program = resolver(draw_op, user);
    crd::gpu::GeometrySource&     g  = out.geometry;
    const containers::StringView  nm = ctx.op_name(draw_op->kind());
    const auto fold = [&](crd::u32 idx, crd::u32& dst) -> bool {
        return draw_op->num_operands() > idx && const_u32(ctx, draw_op->operand(idx), dst);
    };
    if (nm == containers::StringView("render.draw"))
    {
        out.command = crd::gpu::RasterCommandKind::Draw;
        if (!fold(0U, g.vertex_or_index_count) || !fold(1U, g.instance_count)) { return false; }
        g.first_vertex = static_cast<crd::u32>(int_attr(ctx, draw_op, containers::StringView("first_vertex"), 0));
        // ⭐ None (PROCEDURAL — the VS reads gl_VertexIndex, e.g. a fullscreen/proc triangle) vs StoragePull (vertex-PULL
        // from a bound buffer): the binding COUNT is the proxy (a procedural draw binds nothing; a vertex-pull draw binds
        // its buffer). ⛔ the texture-bound-but-procedural edge (bindings present, no vertex buffer) → an explicit
        // geometry-mode attr, named-forward. operands 0-1 are the counts, so >2 operands ⇒ ≥1 binding.
        g.kind = (draw_op->num_operands() > 2U) ? crd::gpu::GeometryKind::StoragePull : crd::gpu::GeometryKind::None;
    }
    else if (nm == containers::StringView("render.draw_indexed"))
    {
        out.command = crd::gpu::RasterCommandKind::DrawIndexed;
        g.kind      = crd::gpu::GeometryKind::Indexed;
        if (!fold(0U, g.vertex_or_index_count) || !fold(1U, g.instance_count)) { return false; }
        g.first_index = static_cast<crd::u32>(int_attr(ctx, draw_op, containers::StringView("first_index"), 0));
        // ⛔ %index_buffer (operand 2) → g.index_buffer via a binding resolver — the caller (14z-1b/2).
    }
    else if (nm == containers::StringView("render.draw_indirect"))
    {
        out.command    = crd::gpu::RasterCommandKind::DrawIndirect;
        g.kind         = crd::gpu::GeometryKind::Indirect;
        g.max_draws    = static_cast<crd::u32>(int_attr(ctx, draw_op, containers::StringView("max_draws"), 1));
        g.args_offset  = static_cast<crd::u64>(int_attr(ctx, draw_op, containers::StringView("args_offset"), 0));
        g.index_offset = static_cast<crd::u32>(int_attr(ctx, draw_op, containers::StringView("index_offset"), 0));
        // ⛔ CEIR-14z-6: g.first_draw_index stays 0 — the DrawIndex BASE is runtime scene state (a group's region base); no
        // authored program pins it. The executor still pushes SV_DrawIndex = 0 + sub-draw. A CEIR consumer = CEIR-15/16.
    }
    else if (nm == containers::StringView("render.draw_indirect_count"))
    {
        out.command    = crd::gpu::RasterCommandKind::DrawIndexedIndirectCount;
        g.kind         = crd::gpu::GeometryKind::IndirectCount;
        g.max_draws    = static_cast<crd::u32>(int_attr(ctx, draw_op, containers::StringView("max_draws"), 1));
        g.args_offset  = static_cast<crd::u64>(int_attr(ctx, draw_op, containers::StringView("args_offset"), 0));
        g.count_offset = static_cast<crd::u64>(int_attr(ctx, draw_op, containers::StringView("count_offset"), 0));
        g.index_offset = static_cast<crd::u32>(int_attr(ctx, draw_op, containers::StringView("index_offset"), 0));
    }
    else if (nm == containers::StringView("render.mesh_dispatch"))
    {
        out.command = crd::gpu::RasterCommandKind::DispatchMesh;
        g.kind      = crd::gpu::GeometryKind::Meshlet;
        if (!fold(0U, g.group_count_x) || !fold(1U, g.group_count_y) || !fold(2U, g.group_count_z)) { return false; }
        // ⛔ CEIR-14z-7: the mesh_dispatch op is 3D (the compute.dispatch mirror), but every Meshlet VERB consumes group_count_x
        // ONLY, so a (gx,gy,gz) with y or z > 1 would SILENTLY lower to a (gx) draw (the cook-only-gates-ship-impossible shape).
        // Refuse a y/z != 1 grid LOUDLY (→ UnsupportedCommand, the dynamic-grid precedent) rather than draw the wrong thing.
        // ⚠ draw_mesh leveled the 3D device APIs (vkCmdDrawMeshTasksEXT / D3D12 DispatchMesh are BOTH 3D) down to 1D — widening
        // the verb to 3D is a signature widen-audit with no y/z consumer yet, so it is a NAMED-FORWARD (the user's call), NOT this slice.
        if (g.group_count_y != 1U || g.group_count_z != 1U) { return false; }
    }
    else if (nm == containers::StringView("render.mesh_dispatch_indirect"))
    {
        out.command   = crd::gpu::RasterCommandKind::DispatchMeshIndirect;
        g.kind        = crd::gpu::GeometryKind::MeshletIndirect;
        g.args_offset = static_cast<crd::u64>(int_attr(ctx, draw_op, containers::StringView("args_offset"), 0));
    }
    else
    {
        return false;
    }
    // CEIR-14z-4c: resolve the draw's variadic binding tail → StorageBuffer bindings. `render_draw_binding_start` gives the
    // tail start (n_counts + n_buffers) from the ONE authoritative per-op layout — never a second table (14c fragility).
    if (binding_resolver != nullptr)
    {
        // CEIR-14z-6: resolve the NAMED command-source buffers (args, count) — the operands BEFORE the binding tail. They feed
        // GeometrySource.args_buffer / count_buffer and do NOT ride out.bindings (they are command-source buffers read by the
        // fixed-function indirect machinery, not descriptor bindings). The FIRST real multi-buffer draw: args + the pull buffer
        // resolve from DISTINCT operands, so the resolver's operand→buffer MAPPING is now observed (a 1-buffer sentinel fails).
        if (g.kind == crd::gpu::GeometryKind::Indirect || g.kind == crd::gpu::GeometryKind::IndirectCount)
        {
            g.args_buffer = binding_resolver(draw_op->operand(0U), binding_user);
            if (g.args_buffer == nullptr) { return false; } // an indirect draw with no args reads garbage — typed fail
            if (g.kind == crd::gpu::GeometryKind::IndirectCount)
            {
                g.count_buffer = binding_resolver(draw_op->operand(1U), binding_user);
                if (g.count_buffer == nullptr) { return false; }
            }
        }
        const crd::u32 start = render_draw_binding_start(nm);
        for (crd::u32 i = start; i < draw_op->num_operands(); ++i)
        {
            crd::gpu::IStorageBuffer* const buf = binding_resolver(draw_op->operand(i), binding_user);
            if (buf == nullptr) { return false; } // a 1-of-N-bound draw renders garbage from the wrong buffer — typed fail
            crd::gpu::ResourceBinding b;
            b.frequency = crd::gpu::BindingFrequency::Object;
            b.kind      = crd::gpu::BindingKind::StorageBuffer;
            b.slot      = i - start; // the binding's ORDINAL (0,1,2…), never constant 0 (a two-buffer draw would alias)
            b.buffer    = buf;
            out.bindings.push_back(b);
        }
    }
    return true;
}

ExecuteError execute_render_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                    crd::gpu::ICommandEncoder& encoder, RasterTargetResolveFn target_resolver,
                                    void* target_user, RasterProgramResolveFn program_resolver, void* program_user,
                                    RasterBindingResolveFn binding_resolver, void* binding_user)
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        switch (cmd.kind)
        {
        case LoweredKind::Barrier:
            break; // ⛔ inert: encoder-surface barriers are the frame graph's (CEIR-15/16, Option A)
        case LoweredKind::BeginRender:
        {
            crd::gpu::RenderingDesc rd;
            if (!materialize_rendering_desc(ctx, cmd.op, target_resolver, target_user, rd))
            {
                return ExecuteError::UnsupportedCommand;
            }
            for (crd::u32 c = 0; c < static_cast<crd::u32>(rd.color.size()); ++c)
            {
                if (rd.color[c].target == nullptr) { return ExecuteError::UnresolvedProgram; }
            }
            if (rd.depth.enabled && rd.depth.target == nullptr) { return ExecuteError::UnresolvedProgram; }
            encoder.begin_rendering(rd);
            break;
        }
        case LoweredKind::Draw:
        {
            crd::gpu::RasterDrawPacket p;
            if (!materialize_draw_packet(ctx, cmd.op, program_resolver, program_user, p, binding_resolver, binding_user))
            {
                return ExecuteError::UnsupportedCommand;
            }
            if (p.program == nullptr) { return ExecuteError::UnresolvedProgram; }
            encoder.draw(p);
            break;
        }
        case LoweredKind::EndRender:
            encoder.end_rendering();
            break;
        case LoweredKind::Dispatch:
        case LoweredKind::Transfer:
            return ExecuteError::UnsupportedCommand; // the render executor is render-only (a mixed program is CEIR-15/16)
        }
    }
    return ExecuteError::None;
}

namespace
{
// CEIR-14z-4a: the per-scope frame-graph pass closure — the CEIR context + the scope's lowered command SLICE + the
// resolvers + the shared frame-recording encoder. Pointers live in a RESERVED array so they stay stable across execute().
struct RenderFrameClosure
{
    const Context*             ctx              = nullptr;
    const LoweredCommand*      begin            = nullptr; // the scope slice [begin, begin+count) = BeginRender…EndRender
    crd::u32                   count            = 0U;
    crd::gpu::ICommandEncoder* encoder          = nullptr;
    RasterTargetResolveFn      target_resolver  = nullptr;
    void*                      target_user      = nullptr;
    RasterProgramResolveFn     program_resolver = nullptr;
    void*                      program_user     = nullptr;
    RasterBindingResolveFn     binding_resolver = nullptr;
    void*                      binding_user     = nullptr;
};
// The frame-graph pass callback: DURING the pass the raster context is in frame-recording mode, so driving the EXISTING
// execute_render_lowered walk on this scope's slice records begin/draw/end into the frame's ONE command buffer (the
// frame-recording verbs then take their real bodies). The imported target IS the raw resolved target inside the pass
// (mirrors crd::rendergraph::execute_frame, which draws to the raw resolved target while the fg tracks the import).
void ceir_render_pass_cb(crd::gpu::IFrameContext& /*fctx*/, void* user)
{
    auto* const c = static_cast<RenderFrameClosure*>(user);
    (void)execute_render_lowered(*c->ctx, containers::ConstSpan<LoweredCommand>(c->begin, c->count), *c->encoder,
                                 c->target_resolver, c->target_user, c->program_resolver, c->program_user,
                                 c->binding_resolver, c->binding_user);
}
} // namespace

ExecuteError execute_render_frame(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                  crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc,
                                  RasterTargetResolveFn target_resolver, void* target_user,
                                  RasterProgramResolveFn program_resolver, void* program_user,
                                  RasterBindingResolveFn binding_resolver, void* binding_user)
{
    auto fg = raster.create_frame_graph();
    if (fg == nullptr) { return ExecuteError::NoFrameGraph; }
    auto encoder = raster.create_command_encoder();
    if (encoder == nullptr) { return ExecuteError::NoFrameGraph; }

    // Count the render.scopes (BeginRender ops) up front, then RESERVE the closure array so the pointers handed to
    // execute() never move on push (the execute_frame stability contract — the callback reads through them at execute()).
    crd::u32 nscopes = 0U;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        if (commands[i].kind == LoweredKind::BeginRender) { ++nscopes; }
    }
    containers::Array<RenderFrameClosure> closures(&alloc);
    closures.reserve(nscopes);

    // ONE frame-graph pass per render.scope: import its targets, declare them `writes` (the fg derives cross-pass barriers
    // + owns end-of-frame readback), and record its command slice through the frame-recording encoder.
    crd::u32 i = 0U;
    while (i < static_cast<crd::u32>(commands.size()))
    {
        if (commands[i].kind != LoweredKind::BeginRender)
        {
            ++i; // a lowered Barrier between scopes is INERT here (the fg derives barriers); skip anything non-scope
            continue;
        }
        const crd::u32 begin_idx = i;
        crd::u32       end_idx   = begin_idx;
        for (crd::u32 j = begin_idx + 1U; j < static_cast<crd::u32>(commands.size()); ++j)
        {
            if (commands[j].kind == LoweredKind::EndRender) { end_idx = j; break; } // NestedRenderScope is verifier-rejected
        }
        const crd::u32 slice_count = end_idx - begin_idx + 1U;

        // Materialize the scope's RenderingDesc to enumerate + import its targets (the raw targets via the caller's resolver).
        crd::gpu::RenderingDesc rd;
        if (!materialize_rendering_desc(ctx, commands[begin_idx].op, target_resolver, target_user, rd))
        {
            return ExecuteError::UnsupportedCommand;
        }
        crd::gpu::IFramePassBuilder& b = fg->add_pass("ceir-scope");
        for (crd::u32 c = 0; c < static_cast<crd::u32>(rd.color.size()); ++c)
        {
            if (rd.color[c].target == nullptr) { return ExecuteError::UnresolvedProgram; }
            b.writes(fg->import_target(*rd.color[c].target));
        }
        if (rd.depth.enabled)
        {
            if (rd.depth.target == nullptr) { return ExecuteError::UnresolvedProgram; }
            b.writes(fg->import_target(*rd.depth.target));
        }

        closures.push_back(RenderFrameClosure{&ctx, &commands[begin_idx], slice_count, encoder.get(), target_resolver,
                                              target_user, program_resolver, program_user, binding_resolver, binding_user});
        b.execute(&ceir_render_pass_cb, &closures[static_cast<crd::usize>(closures.size()) - 1U]);
        i = end_idx + 1U;
    }

    if (!fg->build()) { return ExecuteError::FrameBuildFailed; }
    fg->execute(); // ONE submission — barriers + end-of-frame readback owned by the frame graph
    return ExecuteError::None;
}
} // namespace crd::ceir::gpu
