#include <crd/ceir/gpu/render_materialize.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/scene.hpp> // CEIR-17b: scene::find_scene_misuse (the verifier-first contract for evaluate_scene_resolve)

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
[[nodiscard]] bool bool_attr(const Context& ctx, const Operation* op, containers::StringView name)
{
    const AttrId a = op->attr(name);
    if (!a.valid()) { return false; } // absent ⇒ false (the CEIR-16-3c extent_from_target default)
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::Bool && v.b;
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
    // ⛔ CEIR-16d-live-4a-1: the WBOIT MRT modes — without these the reveal attachment silently folded to Opaque (the
    // materializer's default), so a WBOIT accumulate pass rendered wrong. RevealageMultiply is the ONE the set exists for
    // (dst·(1−src.rgb), which no generic mode expresses); Multiply + RevealComposite complete the enum.
    if (s == containers::StringView("multiply")) { return crd::gpu::BlendMode::Multiply; }
    if (s == containers::StringView("revealage_multiply")) { return crd::gpu::BlendMode::RevealageMultiply; }
    if (s == containers::StringView("reveal_composite")) { return crd::gpu::BlendMode::RevealComposite; }
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
            // ⛔ CEIR-16d LIVE PATH: the depth attachment is a TEMPLATE the record-time target resolver GATES. A forward
            // scene pass whose resolved colour target carries NO bundled depth (and has no explicit/shared depth) resolves
            // to null here — and legacy record_scene_raster leaves `depth` null in exactly that case ⇒ NO depth attachment
            // (it is runtime-dynamic on color->has_depth()). So a NULL resolve DROPS the attachment (depth stays disabled);
            // it is NOT an error. Only a genuinely misconfigured pass — no colour AND no depth — fails, at the
            // zero-attachment guard below. (execute_render_lowered's `depth.enabled && target==nullptr` guard then never
            // fires from this path, because enabled is set only alongside a non-null target.)
            crd::gpu::IRasterTarget* const dt = resolver(att, user);
            if (dt != nullptr)
            {
                out.depth.enabled     = true;
                out.depth.target      = dt;
                out.depth.load        = load_op_of(ctx, att);
                out.depth.store       = store_op_of(ctx, att);
                out.depth.clear_depth = float_attr(ctx, att, containers::StringView("clear_depth"), 1.0F);
                out.depth.compare     = compare_of(ctx, att);
                // ⛔ `read_only` → the per-draw depth-WRITE disable (RasterState), a draw-state concern; named-forward.
            }
        }
        else
        {
            return false;
        }
    }
    // ⛔ CEIR-16d: a scope with ZERO resolved attachments (a depth-only pass whose depth did not resolve, or an empty scope)
    // must NEVER reach begin_rendering — legacy record_scene_raster returns early on `color == nullptr && depth == nullptr`.
    // Fail materialize LOUD (the caller returns UnsupportedCommand) rather than record an empty scope onto the encoder.
    if (out.color.size() == 0U && !out.depth.enabled) { return false; }
    // ⭐ CEIR-16-3c: a composite renders to whatever target it is bound to (a per-cascade shadow atlas, a half-res buffer,
    // the swapchain), whose size is known only at RECORD. `extent_from_target` overrides the authored placeholder width/
    // height with the RESOLVED target's size (record parity: `dims = color != nullptr ? color : depth` — the colour target
    // when present, else the depth target for a DEPTH-ONLY shadow-cascade / prepass scope).
    if (bool_attr(ctx, scope_op, containers::StringView("extent_from_target")))
    {
        const crd::gpu::IRasterTarget* ext = nullptr;
        if (out.color.size() > 0U && out.color[0].target != nullptr) { ext = out.color[0].target; }
        else if (out.depth.enabled && out.depth.target != nullptr) { ext = out.depth.target; }
        if (ext != nullptr)
        {
            out.width  = ext->width();
            out.height = ext->height();
        }
    }
    return true;
}

bool materialize_draw_packet(const Context& ctx, const Operation* draw_op, const RenderResolvers& resolvers,
                             crd::gpu::RasterDrawPacket& out)
{
    out         = crd::gpu::RasterDrawPacket{};
    // ⛔ CEIR-17z: forward the pass's VRS + conservative state onto the draw (the record set these on the resolvers from the
    // payload — shading_rate / conservative). command_lowering reads packet.state for VRS/conservative; a default packet drew
    // at 1x1 regardless of the authored shading_rate. Defaults 1x1/Keep/Off keep ordinary draws byte-identical.
    out.state.vrs_pipeline_rate      = resolvers.vrs_pipeline_rate;
    out.state.vrs_primitive_combiner = resolvers.vrs_primitive_combiner;
    out.state.conservative           = resolvers.conservative;
    out.program = resolvers.program(draw_op, resolvers.program_user);
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
        // ⭐ CEIR-16-3b-1b: the geometry-source mode. An explicit `geometry` attr (procedural|pull) PINS it — REQUIRED for the
        // procedural-with-bindings edge: a fullscreen composite reads TEXTURES yet sources vertices procedurally, and inferring
        // StoragePull would route it to the encoder's vertex-pull arm which, finding NO vertex buffer, draws NOTHING (a black
        // pass). Absent → the 14b heuristic (>2 operands ⇒ a binding present ⇒ StoragePull; else procedural/None).
        const containers::StringView gm = str_attr(ctx, draw_op, containers::StringView("geometry"));
        if (gm == containers::StringView("procedural")) { g.kind = crd::gpu::GeometryKind::None; }
        else if (gm == containers::StringView("pull")) { g.kind = crd::gpu::GeometryKind::StoragePull; }
        else { g.kind = (draw_op->num_operands() > 2U) ? crd::gpu::GeometryKind::StoragePull : crd::gpu::GeometryKind::None; }
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
    // CEIR-14z-4c / CEIR-16: resolve the draw's variadic binding tail. `render_draw_binding_start` gives the tail start
    // (n_counts + n_buffers) from the ONE authoritative per-op layout — never a second table (14c fragility). ⭐ Each binding's
    // KIND derives from its operand's CEIR-3c TYPE (the 12a one-source-of-truth doctrine): a BUFFER-typed operand → a
    // StorageBuffer (`resolvers.storage`); an IMAGE-typed operand → a SampledTexture (`resolvers.texture`); a
    // RESOURCE-TABLE-of-image operand → a BindlessTextureArray (`resolvers.texture_array`, the count behind the resolver).
    if (resolvers.storage != nullptr || resolvers.texture != nullptr || resolvers.texture_array != nullptr)
    {
        // CEIR-14z-6: resolve the NAMED command-source buffers (args, count) — the operands BEFORE the binding tail. They feed
        // GeometrySource.args_buffer / count_buffer and do NOT ride out.bindings (they are command-source buffers read by the
        // fixed-function indirect machinery, not descriptor bindings). Indirect args/count are always BUFFERS → resolvers.storage.
        // ⛔ CEIR-16-mesh-1: MeshletIndirect ALSO reads its group counts from an %args buffer at operand 0 (record_mesh_indirect
        // parity: p.geometry.args_buffer = ctx.storage("args")). It was NOT in this branch (14z wired only args_OFFSET), so the
        // mesh-indirect draw dispatched with a null args buffer — the encoder reads garbage. It has NO count buffer (unlike
        // IndirectCount), just args. render_draw_binding_start(mesh_dispatch_indirect)==1, so the binding tail skips operand 0.
        if (g.kind == crd::gpu::GeometryKind::Indirect || g.kind == crd::gpu::GeometryKind::IndirectCount
            || g.kind == crd::gpu::GeometryKind::MeshletIndirect)
        {
            if (resolvers.storage == nullptr) { return false; } // an indirect draw needs a buffer resolver for its args
            g.args_buffer = resolvers.storage(draw_op->operand(0U), resolvers.storage_user);
            if (g.args_buffer == nullptr) { return false; } // an indirect draw with no args reads garbage — typed fail
            if (g.kind == crd::gpu::GeometryKind::IndirectCount)
            {
                g.count_buffer = resolvers.storage(draw_op->operand(1U), resolvers.storage_user);
                if (g.count_buffer == nullptr) { return false; }
            }
        }
        const crd::u32 start = render_draw_binding_start(nm);
        for (crd::u32 i = start; i < draw_op->num_operands(); ++i)
        {
            const Value* const       operand = draw_op->operand(i);
            const auto&              oty     = ctx.type_of(operand->type());
            const Operation* const   def     = operand->defining_op();
            crd::gpu::ResourceBinding b;
            // ⭐ CEIR-16-3a-2: the binding's SLOT is an explicit `slot` attr on its defining op when present — the fullscreen
            // shadow-atlas binds tex@4 / sampler@5 and a constants buffer@0, none of which is the ordinal position. Absent →
            // the ordinal i-start (the common in-order case). ⛔ absent≠0: a missing attr falls to the ORDINAL, not slot 0.
            b.slot = (def != nullptr) ? static_cast<crd::u32>(int_attr(ctx, def, containers::StringView("slot"),
                                                                       static_cast<crd::i64>(i - start)))
                                      : (i - start);
            if (oty.kind == TypeKind::Image)
            {
                // ⭐ CEIR-16-2: an IMAGE-typed binding operand is a SAMPLED texture. (The shadow-atlas SLOT routing the legacy
                // fullscreen executor used is an ENCODER-dispatch lowering detail — CEIR-16-3a-2/3, expressed as explicit IR.)
                crd::gpu::ITexture* const tex =
                    resolvers.texture != nullptr ? resolvers.texture(operand, resolvers.texture_user) : nullptr;
                if (tex == nullptr) { return false; } // an image binding with no / a failed texture resolver — typed fail, never silent
                b.frequency = crd::gpu::BindingFrequency::Material;
                b.kind      = crd::gpu::BindingKind::SampledTexture;
                b.texture   = tex;
            }
            else if (oty.kind == TypeKind::ResourceTable && oty.members.size() >= 1U
                     && ctx.type_of(oty.members[0]).kind == TypeKind::Image)
            {
                // ⭐ CEIR-16-3a-1: a RESOURCE-TABLE-of-image binding operand is a BINDLESS texture array. The resolver returns
                // the array pointer + writes `count`, so the fullscreen composite's n>1 / blend-load-of-one arms collapse to
                // ONE typed IR binding (the authored-variant plan) — the N-ness never touches the IR (kind-from-type holds).
                crd::u32                         count = 0U;
                crd::gpu::ITexture* const* const arr =
                    resolvers.texture_array != nullptr ? resolvers.texture_array(operand, resolvers.texture_array_user, count)
                                                       : nullptr;
                if (arr == nullptr || count == 0U) { return false; } // a bindless array that does not resolve — typed fail
                b.frequency     = crd::gpu::BindingFrequency::Material;
                b.kind          = crd::gpu::BindingKind::BindlessTextureArray;
                b.texture_array = arr;
                b.array_count   = count;
            }
            else if (oty.kind == TypeKind::Sampler)
            {
                // ⭐ CEIR-16-3a-3: a SAMPLER-typed binding operand is a STANDALONE sampler binding (no texture) — the shadow
                // atlas's slot-5 companion to its slot-4 depth texture. `is_signed` on the CEIR Sampler type = a COMPARISON
                // sampler (⛔⛔⛔ REN-40-D moment-shadow scar: a comparison sampler where the shader declared a plain one — or
                // vice-versa — renders black). NO resolver: the sampler rides its KIND + slot; the device sampler object is
                // the encoder's business (a default SamplerDesc, exactly as bind_atlas leaves it).
                b.frequency = crd::gpu::BindingFrequency::Material;
                b.kind = oty.is_signed ? crd::gpu::BindingKind::ComparisonSampler : crd::gpu::BindingKind::Sampler;
            }
            else
            {
                crd::gpu::IStorageBuffer* const buf =
                    resolvers.storage != nullptr ? resolvers.storage(operand, resolvers.storage_user) : nullptr;
                if (buf == nullptr) { return false; } // a 1-of-N-bound draw renders garbage from the wrong buffer — typed fail
                b.frequency = crd::gpu::BindingFrequency::Object;
                b.kind      = crd::gpu::BindingKind::StorageBuffer;
                b.buffer    = buf;
            }
            out.bindings.push_back(b);
        }
    }
    return true;
}

// ⭐ CEIR-16-mesh-2: expand a `render.mesh_dispatch_list` op over the HOST DrawList into N draws inside the CURRENT scope
// (record_amplify_raster). The op is a TEMPLATE (0 operands): `primitive` picks the verb (meshlet → DispatchMesh / patches →
// DrawPatches — ⛔ the render materializer had NO Patches vocabulary, added here); the per-item program/count/storage come
// from `resolvers.draws`. draws.count==0 → the PROCEDURAL arm: ONE draw of `fallback_count` (0 ⇒ nothing). Mirrors the legacy
// skip semantics EXACTLY: a zero-count item is SKIPPED (never dispatched as one), a per-item program beats the pass default,
// a null-program item is skipped. Returns false on a missing resolver / an unknown `primitive` (a typed fail).
[[nodiscard]] bool emit_amplify_list(const Context& ctx, const Operation* op, crd::gpu::ICommandEncoder& encoder,
                                     const RenderResolvers& resolvers)
{
    if (resolvers.program == nullptr || resolvers.draws_count == nullptr || resolvers.draws_item == nullptr) { return false; }
    const containers::StringView prim = str_attr(ctx, op, containers::StringView("primitive"));
    const bool                   mesh = prim == containers::StringView("meshlet");
    if (!mesh && prim != containers::StringView("patches")) { return false; }
    crd::gpu::IRasterProgram* const def_prog = resolvers.program(op, resolvers.program_user);

    const auto emit = [&](crd::gpu::IRasterProgram* prog, crd::u32 count, crd::gpu::IStorageBuffer* geo)
    {
        crd::gpu::RasterDrawPacket p;
        p.program = prog;
        if (mesh)
        {
            p.command                = crd::gpu::RasterCommandKind::DispatchMesh;
            p.geometry.kind          = crd::gpu::GeometryKind::Meshlet;
            p.geometry.group_count_x = count;
        }
        else
        {
            p.command              = crd::gpu::RasterCommandKind::DrawPatches;
            p.geometry.kind        = crd::gpu::GeometryKind::Patches;
            p.geometry.patch_count = count;
        }
        if (geo != nullptr)
        {
            p.bindings.push_back(crd::gpu::ResourceBinding{crd::gpu::BindingFrequency::Object,
                                                          crd::gpu::BindingKind::StorageBuffer, 0U, geo});
        }
        encoder.draw(p);
    };

    const crd::u32 n = resolvers.draws_count(resolvers.draws_user);
    if (n == 0U)
    {
        const crd::i64 fc = int_attr(ctx, op, containers::StringView("fallback_count"), 0);
        if (fc > 0 && def_prog != nullptr) { emit(def_prog, static_cast<crd::u32>(fc), nullptr); }
        return true;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        RasterDrawItem it{};
        resolvers.draws_item(resolvers.draws_user, i, it);
        if (it.vertex_count == 0U) { continue; } // ⛔ a zero-count item is SKIPPED, never dispatched (record_amplify_raster)
        crd::gpu::IRasterProgram* const prog = it.program != nullptr ? it.program : def_prog;
        if (prog == nullptr) { continue; }
        emit(prog, it.vertex_count, it.storage);
    }
    return true;
}

// ── CEIR-16d: the SCENE per-item sampled-texture binding — a byte-exact replica of record_scene_raster's
// bind_map / bind_atlas / attach_textures. ⛔⛔ LAYERING: ceir-gpu cannot call render-graph's frame_graph.cpp, so the scar
// surface is reproduced here; the two MUST bind identically for A/B parity. ⛔⛔⛔ atlas-by-SLOT-4 + its sampler COMPANION at
// slot 5; the base-colour map at slot 1; ⛔⛔ REN-40-D: a COMPARISON sampler for a DEPTH atlas (a PCF shadow lookup), a PLAIN
// filtering sampler for a moment/variance COLOUR array — the wrong one renders every moment shadow black.
void scene_bind_map(crd::gpu::RasterDrawPacket& pk, crd::gpu::ITexture* tex)
{
    pk.bindings.push_back(crd::gpu::ResourceBinding{crd::gpu::BindingFrequency::Material,
                                                    crd::gpu::BindingKind::SampledTexture, 1U, nullptr, tex});
}
void scene_bind_atlas(crd::gpu::RasterDrawPacket& pk, crd::gpu::ITexture* tex, bool comparison)
{
    pk.bindings.push_back(crd::gpu::ResourceBinding{crd::gpu::BindingFrequency::Material,
                                                    crd::gpu::BindingKind::SampledTexture, 4U, nullptr, tex});
    crd::gpu::ResourceBinding samp{};
    samp.frequency = crd::gpu::BindingFrequency::Material;
    samp.kind      = comparison ? crd::gpu::BindingKind::ComparisonSampler : crd::gpu::BindingKind::Sampler;
    samp.slot      = 5U;
    pk.bindings.push_back(samp);
}
void scene_attach_textures(crd::gpu::RasterDrawPacket& pk, crd::gpu::ITexture* item_tex, crd::gpu::ITexture* pass_tex,
                           crd::gpu::ITexture* tex, bool combined, bool depth_tex, bool comparison)
{
    if (combined)
    {
        scene_bind_map(pk, item_tex);
        scene_bind_atlas(pk, pass_tex, comparison);
    }
    else if (depth_tex)
    {
        scene_bind_atlas(pk, tex, comparison);
    }
    else if (tex != nullptr)
    {
        scene_bind_map(pk, tex);
    }
}

// ⭐ CEIR-16d: expand a `render.scene_draw_list` op over the HOST DrawList into N raster packets inside the CURRENT scope —
// a VERBATIM replica of record_scene_raster's single-colour draw loop (the mrt>=2 G-buffer arm is a SEPARATE representation).
// The op is a TEMPLATE (0 operands): every per-item field (program/storage/texture/args/counts) is DrawList data resolved via
// `resolvers.draws_*`; the pass-level atlas via `resolvers.pass_texture`; the colour-attachment presence via `scope_has_color`
// (⛔ ADVISOR GAP-1: a DEPTH-ONLY pass binds NO textures — the walk hands the MATERIALIZED scope's colour count, never an op
// attr). ⛔ the per-item VERB is chosen exactly as the live RasterGeometry selection: indirect → indexed-sampled → combined/
// textured → PLAIN with a COALESCED run (⛔ ASYMMETRIES preserved: first_draw_index rides indirect/indexed/multi but NOT
// combined-tex/single-plain; run>1||indexed → DrawMulti; i += run-1; the same-storage run predicate). Returns false only on a
// missing required resolver (the program/draws accessors).
[[nodiscard]] bool emit_scene_list(const Context& ctx, const Operation* op, crd::gpu::ICommandEncoder& encoder,
                                   const RenderResolvers& resolvers, bool scope_has_color)
{
    if (resolvers.program == nullptr || resolvers.draws_count == nullptr || resolvers.draws_item == nullptr) { return false; }
    crd::gpu::IRasterProgram* const def_prog = resolvers.program(op, resolvers.program_user);

    // ⛔ CEIR-16z-2: PROCEDURAL mode (the §41 visbuffer dissolution) — a byte-exact port of the deleted record_visbuffer_raster
    // draw loop. Each item is a plain gl_VertexIndex Draw (GeometryKind::None, vertex_count, NO storage binding, no textures,
    // no coalescing). ⛔ the skip is on vertex_count==0 (NOT storage — a procedural draw HAS no storage, and the storage-null
    // skip below is the RESOLVE-FAILURE guard, a different concern). The pass-texture / scope_has_color routing is STORAGE-only.
    if (str_attr(ctx, op, containers::StringView("geometry")) == containers::StringView("procedural"))
    {
        const crd::u32 np = resolvers.draws_count(resolvers.draws_user);
        for (crd::u32 i = 0; i < np; ++i)
        {
            RasterDrawItem it{};
            resolvers.draws_item(resolvers.draws_user, i, it);
            if (it.vertex_count == 0U) { continue; }
            crd::gpu::IRasterProgram* const prog = it.program != nullptr ? it.program : def_prog;
            if (prog == nullptr) { continue; }
            crd::gpu::RasterDrawPacket p;
            p.program                        = prog;
            p.command                        = crd::gpu::RasterCommandKind::Draw;
            p.geometry.kind                  = crd::gpu::GeometryKind::None; // a procedural VS (gl_VertexIndex); the id target IS the scope
            p.geometry.vertex_or_index_count = it.vertex_count;
            encoder.draw(p); // ZERO bindings — the visbuffer VS pulls nothing
        }
        return true;
    }

    // The PASS-level sampled atlas (shadow / moment), resolved once. pass_depth is meaningless without a pass_tex (a
    // depth-only cascade reads NOTHING); batchable_pass: a pass that reads a texture never coalesces runs.
    bool                      pass_is_depth = false;
    bool                      pass_is_cmp   = false;
    crd::gpu::ITexture* const pass_tex      = resolvers.pass_texture != nullptr
                                                  ? resolvers.pass_texture(resolvers.pass_texture_user, pass_is_depth, pass_is_cmp)
                                                  : nullptr;
    const bool pass_depth     = pass_tex != nullptr && pass_is_depth;
    const bool pass_cmp       = pass_is_cmp;
    const bool batchable_pass = pass_tex == nullptr;

    const crd::u32 n = resolvers.draws_count(resolvers.draws_user);
    // count==0: the scope's Begin/End (materialized by the walk) still runs, so the pass CLEARS regardless; the legacy
    // geometry-slot fullscreen-triangle fallback is a TEST-gate shape (no shipped scene pass records an empty draw list),
    // deferred to the gpu-test conversion.
    for (crd::u32 i = 0; i < n; ++i)
    {
        RasterDrawItem it{};
        resolvers.draws_item(resolvers.draws_user, i, it);
        if (it.storage == nullptr) { continue; }
        crd::gpu::IRasterProgram* const prog = it.program != nullptr ? it.program : def_prog;
        if (prog == nullptr) { continue; }

        // A draw's OWN texture (base-colour map) beats the pass atlas; a pass depth read with no per-item map is a shadow
        // lookup; a per-item map INSIDE a depth-reading pass is the COMBINED shape. ⛔ a DEPTH-ONLY pass (no colour) binds
        // NO textures — a colour verb here would misrender or drop the depth write.
        const bool                has_color = scope_has_color;
        crd::gpu::ITexture* const item_map  = has_color ? it.texture : nullptr;
        crd::gpu::ITexture*       tex        = nullptr;
        if (has_color) { tex = item_map != nullptr ? item_map : pass_tex; }
        const bool                     depth_tex = has_color && item_map == nullptr && pass_depth;
        const bool                     combined  = has_color && item_map != nullptr && pass_tex != nullptr && pass_depth;
        const crd::gpu::ResourceBinding sbind{crd::gpu::BindingFrequency::Object, crd::gpu::BindingKind::StorageBuffer, 0U,
                                              it.storage};

        // GPU-DRIVEN indirect (count in device memory): DrawIndex ROW = i; never falls to a CPU-count verb.
        if (it.args != nullptr && it.index_count > 0U)
        {
            crd::gpu::RasterDrawPacket p;
            p.program                   = prog;
            p.command                   = crd::gpu::RasterCommandKind::DrawIndexedIndirect;
            p.geometry.kind             = crd::gpu::GeometryKind::Indirect;
            p.geometry.args_buffer      = it.args;
            p.geometry.args_offset      = it.args_offset;
            p.geometry.max_draws        = 1U;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);
            scene_attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp);
            encoder.draw(p);
            continue;
        }
        // INDEXED-PULL carrying per-draw texture state → the indexed SAMPLED verb (one verb, DrawIndex row = i).
        if (it.index_count > 0U && (tex != nullptr || depth_tex))
        {
            crd::gpu::RasterDrawPacket p;
            p.program                        = prog;
            p.command                        = crd::gpu::RasterCommandKind::DrawIndexed;
            p.geometry.kind                  = crd::gpu::GeometryKind::Indexed;
            p.geometry.vertex_or_index_count = it.index_count;
            p.geometry.instance_count        = it.instance_count;
            p.geometry.first_index           = it.first_index;
            p.geometry.first_draw_index      = i;
            p.bindings.push_back(sbind);
            scene_attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp);
            encoder.draw(p);
            continue;
        }
        // COMBINED / SHADOWED / TEXTURED (non-indexed) — a single StoragePull draw carrying the resolved textures.
        if (combined || (tex != nullptr))
        {
            crd::gpu::RasterDrawPacket p;
            p.program                        = prog;
            p.command                        = crd::gpu::RasterCommandKind::Draw;
            p.geometry.kind                  = crd::gpu::GeometryKind::StoragePull;
            p.geometry.vertex_or_index_count = it.vertex_count;
            p.bindings.push_back(sbind);
            scene_attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp);
            encoder.draw(p);
            continue;
        }

        // PLAIN: coalesce a RUN of consecutive plain items (same program+storage, no texture, same indexed-ness) into ONE
        // multi verb — the batching perf contract (one descriptor reset per run, not per draw).
        crd::u32 run = 1U;
        if (batchable_pass && it.texture == nullptr)
        {
            while (i + run < n && run < kMaxSceneRun)
            {
                RasterDrawItem nx{};
                resolvers.draws_item(resolvers.draws_user, i + run, nx);
                crd::gpu::IRasterProgram* const nprog = nx.program != nullptr ? nx.program : def_prog;
                if (nx.storage == nullptr || nx.texture != nullptr || nx.args != nullptr || nprog != prog ||
                    nx.indexed != it.indexed || (nx.index_count > 0U) != (it.index_count > 0U) || nx.storage != it.storage)
                {
                    break;
                }
                ++run;
            }
        }
        if (it.index_count > 0U)
        {
            // an INDEXED-PULL run is ONE indexed-multi command; a run of one still routes here (the multi verb pushes the
            // DrawIndex row a rebased indexed program needs).
            crd::gpu::IRasterContext::IndexedDraw idraws[kMaxSceneRun];
            for (crd::u32 k = 0; k < run; ++k)
            {
                RasterDrawItem nk{};
                resolvers.draws_item(resolvers.draws_user, i + k, nk);
                idraws[k] = {nk.index_count, nk.instance_count, nk.first_index};
            }
            crd::gpu::RasterDrawPacket p;
            p.program                   = prog;
            p.command                   = crd::gpu::RasterCommandKind::DrawMultiIndexed;
            p.geometry.kind             = crd::gpu::GeometryKind::MultiIndexed;
            p.geometry.multi_indexed    = static_cast<const crd::gpu::IRasterContext::IndexedDraw*>(idraws);
            p.geometry.draw_count       = run;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);
            encoder.draw(p);
            i += run - 1U;
        }
        else if (run > 1U || it.indexed)
        {
            // a non-indexed run (or a single item flagged `indexed` — its program rebases loads by DrawIndex, and only the
            // multi verb pushes the row).
            crd::u32 counts[kMaxSceneRun];
            for (crd::u32 k = 0; k < run; ++k)
            {
                RasterDrawItem nk{};
                resolvers.draws_item(resolvers.draws_user, i + k, nk);
                counts[k] = nk.vertex_count;
            }
            crd::gpu::RasterDrawPacket p;
            p.program                   = prog;
            p.command                   = crd::gpu::RasterCommandKind::DrawMulti;
            p.geometry.kind             = crd::gpu::GeometryKind::MultiStoragePull;
            p.geometry.multi_counts     = static_cast<const crd::u32*>(counts);
            p.geometry.draw_count       = run;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);
            encoder.draw(p);
            i += run - 1U;
        }
        else
        {
            // a single plain item.
            crd::gpu::RasterDrawPacket p;
            p.program                        = prog;
            p.command                        = crd::gpu::RasterCommandKind::Draw;
            p.geometry.kind                  = crd::gpu::GeometryKind::StoragePull;
            p.geometry.vertex_or_index_count = it.vertex_count;
            p.bindings.push_back(sbind);
            encoder.draw(p);
        }
    }
    return true;
}

// ⛔ CEIR-16d-live-4a-3: the MULTI-COLOUR MRT expansion (a deferred G-buffer / WBOIT accumulate). A DIFFERENT record SHAPE
// from the single-colour scene ladder: the legacy record_scene_raster MRT arm (frame_graph.cpp L365-399) opens a SCOPE PER
// ITEM — begin(rd) / one clearing StoragePull draw / end each (the encoder's clear-once makes item 0 clear + the rest load, so
// Additive accum + RevealageMultiply reveal accumulate). NO textures, NO coalescing — a G-buffer/WBOIT item is a plain
// StoragePull. The IR is unchanged (ONE render.scope); this per-item begin/end is a record-time LOWERING artifact, which is
// why the walk DEFERS the scope's begin_rendering to here (execute_render_lowered, ≥2-colour BeginRender). `rd` is the
// materialized ≥2-colour scope (all Clear — the MRT arm IGNORES load, gap iv). Byte-identical to legacy L377-398.
[[nodiscard]] bool emit_scene_list_mrt(const Operation* op, crd::gpu::ICommandEncoder& encoder,
                                       const RenderResolvers& resolvers, const crd::gpu::RenderingDesc& rd)
{
    if (resolvers.program == nullptr || resolvers.draws_count == nullptr || resolvers.draws_item == nullptr) { return false; }
    crd::gpu::IRasterProgram* const def_prog = resolvers.program(op, resolvers.program_user);
    // ⛔⛔ CEIR-18c: a ≥2-colour MRT scene list is a DEFERRED G-BUFFER — its items are the SAME DrawList shapes the
    // single-colour ladder draws (REN-39 INDEXED-PULL, REN-40 GPU-indirect, textured), NOT only the non-indexed
    // storage-pull the WBOIT / 14z-4c single-draw cases used. The original arm hardcoded `Draw` + `vertex_count`,
    // so an INDEXED G-buffer item (index_count>0, vertex_count IGNORED=0) drew ZERO vertices → 0 fragments, and the
    // whole G-buffer stayed at its clear. Select the per-item verb exactly as emit_scene_list (minus coalescing —
    // the MRT arm opens a scope PER ITEM). The non-indexed branch below is byte-identical to the legacy MRT arm.
    bool                      pass_is_depth = false;
    bool                      pass_is_cmp   = false;
    crd::gpu::ITexture* const pass_tex      = resolvers.pass_texture != nullptr
                                                  ? resolvers.pass_texture(resolvers.pass_texture_user, pass_is_depth, pass_is_cmp)
                                                  : nullptr;
    const bool     pass_depth = pass_tex != nullptr && pass_is_depth;
    const bool     pass_cmp   = pass_is_cmp;
    const crd::u32 n          = resolvers.draws_count(resolvers.draws_user);
    for (crd::u32 i = 0; i < n; ++i)
    {
        RasterDrawItem it{};
        resolvers.draws_item(resolvers.draws_user, i, it);
        if (it.storage == nullptr) { continue; }
        crd::gpu::IRasterProgram* const prog = it.program != nullptr ? it.program : def_prog;
        if (prog == nullptr) { continue; }
        // a G-buffer scope always has colour; a per-item base-colour map beats the (usually absent) pass atlas.
        crd::gpu::ITexture* const item_map = it.texture;
        crd::gpu::ITexture* const tex      = item_map != nullptr ? item_map : pass_tex;
        const bool                depth_tex = item_map == nullptr && pass_depth;
        const bool                combined  = item_map != nullptr && pass_tex != nullptr && pass_depth;
        const crd::gpu::ResourceBinding sbind{crd::gpu::BindingFrequency::Object, crd::gpu::BindingKind::StorageBuffer, 0U,
                                              it.storage};
        encoder.begin_rendering(rd);
        crd::gpu::RasterDrawPacket p;
        p.program = prog;
        if (it.args != nullptr && it.index_count > 0U)
        {
            // GPU-driven indirect (the command count is in device memory; DrawIndex row = i).
            p.command                   = crd::gpu::RasterCommandKind::DrawIndexedIndirect;
            p.geometry.kind             = crd::gpu::GeometryKind::Indirect;
            p.geometry.args_buffer      = it.args;
            p.geometry.args_offset      = it.args_offset;
            p.geometry.max_draws        = 1U;
            p.geometry.first_draw_index = i;
        }
        else if (it.index_count > 0U)
        {
            // INDEXED-PULL (the DrawIndex row a rebased indexed program needs rides `first_draw_index`).
            p.command                        = crd::gpu::RasterCommandKind::DrawIndexed;
            p.geometry.kind                  = crd::gpu::GeometryKind::Indexed;
            p.geometry.vertex_or_index_count = it.index_count;
            p.geometry.instance_count        = it.instance_count;
            p.geometry.first_index           = it.first_index;
            p.geometry.first_draw_index      = i;
        }
        else
        {
            // NON-INDEXED storage-pull (the WBOIT / 14z-4c legacy MRT shape) — byte-identical to before.
            p.command                        = crd::gpu::RasterCommandKind::Draw;
            p.geometry.kind                  = crd::gpu::GeometryKind::StoragePull;
            p.geometry.vertex_or_index_count = it.vertex_count;
        }
        p.bindings.push_back(sbind);
        if (tex != nullptr || depth_tex || combined) { scene_attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp); }
        encoder.draw(p);
        encoder.end_rendering();
    }
    return true;
}

ExecuteError execute_render_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                    crd::gpu::ICommandEncoder& encoder, const RenderResolvers& resolvers)
{
    // ⛔ ADVISOR GAP-1: the scene verb ladder's texture routing gates on COLOUR-attachment presence (a depth-only pass binds
    // NO textures), but the Draw case has no scope state. Hoist it from the MATERIALIZED scope (rd.color.size()>0) at
    // BeginRender — the scope is the source of truth, never a second op attr the builder could get wrong.
    bool scope_has_color = false;
    // ⛔ CEIR-16d-live-4a-3: MRT deferral state. A ≥2-colour scope (a deferred G-buffer / WBOIT accumulate) opens a scope PER
    // ITEM (emit_scene_list_mrt — the legacy record_scene_raster MRT shape), so its begin_rendering is DEFERRED from
    // BeginRender to the draw. `deferred_rd` stashes the materialized scope; `outer_begin_open` tracks whether an OUTER
    // begin_rendering is open + needs an EndRender close (single-colour scopes, and the deferred-then-non-scene-draw fallback).
    bool                    deferred         = false;
    bool                    outer_begin_open = false;
    crd::gpu::RenderingDesc deferred_rd;
    // Issue a DEFERRED (≥2-colour MRT) scope's begin_rendering NOW — used when a NON-scene-draw op (an amplify list, or the
    // 14z-4c gbuffer single render.draw) records inside a ≥2-colour scope: those keep the ONE-scope-N-draws shape, so the
    // deferred begin must open before they record (⛔ mandatory or the 14z-4c single-draw MRT device tests break).
    const auto flush_deferred = [&]() {
        if (deferred)
        {
            encoder.begin_rendering(deferred_rd);
            deferred         = false;
            outer_begin_open = true;
        }
    };
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
            if (!materialize_rendering_desc(ctx, cmd.op, resolvers.target, resolvers.target_user, rd))
            {
                return ExecuteError::UnsupportedCommand;
            }
            for (crd::u32 c = 0; c < static_cast<crd::u32>(rd.color.size()); ++c)
            {
                if (rd.color[c].target == nullptr) { return ExecuteError::UnresolvedProgram; }
            }
            if (rd.depth.enabled && rd.depth.target == nullptr) { return ExecuteError::UnresolvedProgram; }
            scope_has_color = rd.color.size() > 0U;
            // ⛔ CEIR-16d-live-4a-3: a ≥2-colour MRT scope DEFERS its begin_rendering — emit_scene_list_mrt runs the per-item
            // begin/draw/end at the Draw (the legacy MRT arm's scope-per-item shape). GAP (iv): the MRT arm hardcodes Clear
            // (IGNORES load), so force_load is NOT applied here — the 4a-2 builder already baked Clear. force_load stays a
            // SINGLE-colour concern.
            if (rd.color.size() >= 2U)
            {
                deferred         = true;
                deferred_rd      = rd;
                outer_begin_open = false;
                break;
            }
            // ⛔ CEIR-16d-live-2b: the PER-INSTANCE load override. The static plan baked only the AUTHORED base load (kLoad);
            // the FRAME-VARYING per-for_each-instance `load_override` (a cached shadow cascade PRESERVES its atlas layer,
            // REN-40-E2) rides via resolvers.force_load. Force every colour + the depth LoadOp to Load — MONOTONE
            // (Clear→Load only; Store + the clear values UNTOUCHED), matching legacy record_scene_raster (colour load =
            // `load`; depth load = `load ‖ load_depth`, so depth loads whenever colour does). Idempotent when the base
            // already baked Load (static kLoad / kComposite).
            if (resolvers.force_load)
            {
                for (crd::u32 c = 0; c < static_cast<crd::u32>(rd.color.size()); ++c)
                {
                    rd.color[c].load = crd::gpu::LoadOp::Load;
                }
                if (rd.depth.enabled) { rd.depth.load = crd::gpu::LoadOp::Load; }
            }
            deferred         = false;
            outer_begin_open = true;
            encoder.begin_rendering(rd);
            break;
        }
        case LoweredKind::Draw:
        {
            // ⭐ CEIR-16-mesh-2: a per-draw-item AMPLIFICATION (render.mesh_dispatch_list) expands over the host DrawList into
            // N draws in THIS scope — its per-item program/count/storage are DrawList data, not op operands (a 0-operand op),
            // so materialize_draw_packet (single-op-operands) does not apply.
            if (ctx.op_name(cmd.op->kind()) == containers::StringView("render.mesh_dispatch_list"))
            {
                flush_deferred(); // an amplify list keeps the one-scope-N-draws shape (never a per-item MRT scope)
                if (!emit_amplify_list(ctx, cmd.op, encoder, resolvers)) { return ExecuteError::UnsupportedCommand; }
                break;
            }
            // ⭐ CEIR-16d: the SCENE per-draw-item verb ladder (render.scene_draw_list) — likewise a 0-operand TEMPLATE
            // expanding over the host DrawList, but the verb is chosen per-item + it needs the scope's colour presence.
            if (ctx.op_name(cmd.op->kind()) == containers::StringView("render.scene_draw_list"))
            {
                // ⛔ CEIR-16d-live-4a-3: a DEFERRED (≥2-colour MRT) scene list is the per-item MRT expansion (begin/draw/end
                // per item — the legacy record_scene_raster MRT arm); it opens + closes its OWN scopes, so `outer_begin_open`
                // stays false (EndRender emits nothing). The single-colour scene ladder is unchanged.
                if (deferred)
                {
                    if (!emit_scene_list_mrt(cmd.op, encoder, resolvers, deferred_rd))
                    {
                        return ExecuteError::UnsupportedCommand;
                    }
                    deferred = false; // consumed; the per-item scopes self-closed (no outer begin/end)
                    break;
                }
                if (!emit_scene_list(ctx, cmd.op, encoder, resolvers, scope_has_color))
                {
                    return ExecuteError::UnsupportedCommand;
                }
                break;
            }
            flush_deferred(); // a single materialized draw (14z-4c gbuffer single MRT) keeps the one-scope shape
            crd::gpu::RasterDrawPacket p;
            if (!materialize_draw_packet(ctx, cmd.op, resolvers, p))
            {
                return ExecuteError::UnsupportedCommand;
            }
            if (p.program == nullptr) { return ExecuteError::UnresolvedProgram; }
            encoder.draw(p);
            break;
        }
        case LoweredKind::EndRender:
            // ⛔ CEIR-16d-live-4a-3: close the OUTER scope only if one is open — a consumed MRT scene list (per-item scopes
            // self-closed) and an UNRESOLVED deferral (a ≥2-colour scope with 0 items — matches legacy's 0-iteration loop:
            // no begin, no end) both leave outer_begin_open false, so nothing is emitted here.
            if (outer_begin_open) { encoder.end_rendering(); }
            scope_has_color  = false;
            deferred         = false;
            outer_begin_open = false;
            break;
        case LoweredKind::Dispatch:
        case LoweredKind::Transfer:
        case LoweredKind::RayQuery:   // CEIR-19c: ceir.rt kinds target execute_rt_lowered, not the raster surface
        case LoweredKind::AccelBuild:
        case LoweredKind::DispatchIndirect: // CEIR-20b: ceir.work targets execute_work_lowered, not the raster surface
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
    const Context*             ctx     = nullptr;
    const LoweredCommand*      begin   = nullptr; // the scope slice [begin, begin+count) = BeginRender…EndRender
    crd::u32                   count   = 0U;
    crd::gpu::ICommandEncoder* encoder = nullptr;
    RenderResolvers            resolvers; // CEIR-16: the four bundled resolvers (was four separate fn/user pairs)
};
// The frame-graph pass callback: DURING the pass the raster context is in frame-recording mode, so driving the EXISTING
// execute_render_lowered walk on this scope's slice records begin/draw/end into the frame's ONE command buffer (the
// frame-recording verbs then take their real bodies). The imported target IS the raw resolved target inside the pass
// (mirrors crd::rendergraph::execute_frame, which draws to the raw resolved target while the fg tracks the import).
void ceir_render_pass_cb(crd::gpu::IFrameContext& /*fctx*/, void* user)
{
    auto* const c = static_cast<RenderFrameClosure*>(user);
    (void)execute_render_lowered(*c->ctx, containers::ConstSpan<LoweredCommand>(c->begin, c->count), *c->encoder,
                                 c->resolvers);
}
} // namespace

ExecuteError execute_render_frame(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                  crd::gpu::IRasterContext& raster, crd::memory::IAllocator& alloc,
                                  const RenderResolvers& resolvers)
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
        if (!materialize_rendering_desc(ctx, commands[begin_idx].op, resolvers.target, resolvers.target_user, rd))
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

        closures.push_back(RenderFrameClosure{&ctx, &commands[begin_idx], slice_count, encoder.get(), resolvers});
        b.execute(&ceir_render_pass_cb, &closures[static_cast<crd::usize>(closures.size()) - 1U]);
        i = end_idx + 1U;
    }

    if (!fg->build()) { return ExecuteError::FrameBuildFailed; }
    fg->execute(); // ONE submission — barriers + end-of-frame readback owned by the frame graph
    return ExecuteError::None;
}

// ── CEIR-17b: the scene.resolve_* chain evaluator ─────────────────────────────────────────────────────────────────
namespace
{
// the value→handle binding map for a resolve chain (the seed draw + each resolved result). A chain is a handful of ops,
// so a small fixed map (no allocator needed) — the emit_scene_list stack-array precedent.
struct SceneEvalMap
{
    struct Bind
    {
        const Value*       v = nullptr;
        SceneResolveHandle h = 0;
    };
    static constexpr crd::u32 kMax = 64U;
    Bind                      binds[kMax];
    crd::u32                  n = 0U;

    [[nodiscard]] SceneResolveHandle lookup(const Value* v) const noexcept
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (binds[i].v == v) { return binds[i].h; }
        }
        return 0U; // an unbound operand ⇒ 0 (an unresolvable upstream — the callback receives 0)
    }
    void bind(const Value* v, SceneResolveHandle h) noexcept
    {
        if (n < kMax) { binds[n++] = {v, h}; }
    }
};

// The pre-order walk: for each scene.resolve_* op, look up its RESOLVED upstream operand handle(s), call the matching
// callback, bind the op-result. A null callback the chain needs ⇒ UnresolvedSceneHandle; a 0 return (an unresolvable
// handle) likewise. The op order is SSA (defs precede uses), so a linear walk resolves the chain in one pass.
ExecuteError eval_scene_region(const Context& ctx, const Region* r, const RenderResolvers& res, SceneEvalMap& map,
                               SceneResolvedHandles& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return ExecuteError::None; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            if (nm == containers::StringView("scene.resolve_material"))
            {
                if (res.resolve_material == nullptr) { return ExecuteError::UnresolvedSceneHandle; }
                const SceneResolveHandle mh = res.resolve_material(res.resolve_material_user, map.lookup(op->operand(0U)));
                if (mh == 0U) { return ExecuteError::UnresolvedSceneHandle; }
                map.bind(op->result(0U), mh);
                out.material = mh;
            }
            else if (nm == containers::StringView("scene.resolve_technique"))
            {
                if (res.resolve_technique == nullptr) { return ExecuteError::UnresolvedSceneHandle; }
                const SceneResolveHandle th = res.resolve_technique(res.resolve_technique_user, map.lookup(op->operand(0U)),
                                                                    str_attr(ctx, op, containers::StringView("phase")));
                if (th == 0U) { return ExecuteError::UnresolvedSceneHandle; }
                map.bind(op->result(0U), th);
                out.technique = th;
            }
            else if (nm == containers::StringView("scene.resolve_program"))
            {
                if (res.resolve_program == nullptr) { return ExecuteError::UnresolvedSceneHandle; }
                const SceneResolveHandle pg = res.resolve_program(res.resolve_program_user, map.lookup(op->operand(0U)),
                                                                  map.lookup(op->operand(1U)));
                if (pg == 0U) { return ExecuteError::UnresolvedSceneHandle; }
                map.bind(op->result(0U), pg);
                out.program = pg;
            }
            else if (nm == containers::StringView("scene.resolve_geometry"))
            {
                if (res.resolve_geometry == nullptr) { return ExecuteError::UnresolvedSceneHandle; }
                const SceneResolveHandle gh = res.resolve_geometry(res.resolve_geometry_user, map.lookup(op->operand(0U)));
                if (gh == 0U) { return ExecuteError::UnresolvedSceneHandle; }
                map.bind(op->result(0U), gh);
                out.geometry = gh;
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const ExecuteError e = eval_scene_region(ctx, op->region(i), res, map, out);
                if (e != ExecuteError::None) { return e; }
            }
        }
    }
    return ExecuteError::None;
}
} // namespace

ExecuteError evaluate_scene_resolve(Context& ctx, const Module& m, const RenderResolvers& resolvers,
                                    const Value* draw_seed, SceneResolveHandle draw_handle, SceneResolvedHandles& out)
{
    out = SceneResolvedHandles{};
    // ⛔ VERIFIER-FIRST: a mis-typed chain refuses BEFORE any callback runs (never a garbage handle) — the same contract
    // execute_render_lowered assumes (find_render_misuse passed).
    if (crd::ceir::scene::find_scene_misuse(ctx, m).kind != crd::ceir::scene::SceneMisuseKind::None)
    {
        return ExecuteError::SceneChainMisuse;
    }
    SceneEvalMap map;
    map.bind(draw_seed, draw_handle); // the chain's scene.draw INPUT resolves to the caller's draw handle
    return eval_scene_region(ctx, m.body(), resolvers, map, out);
}
} // namespace crd::ceir::gpu
