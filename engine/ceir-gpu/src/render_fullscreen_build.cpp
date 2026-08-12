// CEIR-16-3b: the fullscreen composite builder (see render_fullscreen_build.hpp). Emits a `render.scope` with ONE procedural
// 3-vertex draw over the declared reads, verifies with `find_render_misuse`, and lowers to a `LoweredCommand` plan. ⛔ The
// BINDING KIND is never chosen here as an encoder call (the record_fullscreen_raster shape) — it is chosen by the CEIR TYPE of
// each declared read (Image → SampledTexture, ResourceTable(Image) → BindlessTextureArray, Sampler → Comparison/Sampler:
// CEIR-16-2/3a), so `execute_render_lowered` materializes it generically. This tick (16b-3b-1) emits the PROCEDURAL (no reads)
// and PLAIN-SAMPLED (each read → an Image binding at its ordinal slot) shapes; the shadow-atlas (tex@4 + comparison sampler@5)
// and bindless (ResourceTable) routing land in 16b-3b-2/3 as additions keyed on is_depth / load+blend.

#include <crd/ceir/gpu/render_fullscreen_build.hpp>

#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/render.hpp> // render::register_dialect + render::type_color_attachment

namespace crd::ceir::gpu
{
bool build_fullscreen_ceir(Context& ctx, const FullscreenBuildDesc& desc, containers::Array<LoweredCommand>& out_plan)
{
    out_plan.clear();
    if (desc.num_inputs > 8U) { return false; } // the fixed input0..input7 fan (record_fullscreen_raster's kInputs)

    // The composite uses the arith (const counts), resource (declared reads/target), and render (scope/draw/attachment)
    // dialects. Registration is idempotent on a fresh Context (the documented precondition).
    (void)arith::register_arith_ops(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)render::register_dialect(ctx);

    const OpId k_cst   = ctx.intern_op("arith", "const");
    const OpId k_decl  = ctx.intern_op("resource", "declare");
    const OpId k_col   = ctx.intern_op("render", "color_attachment");
    const OpId k_scope = ctx.intern_op("render", "scope");
    const OpId k_draw  = ctx.intern_op("render", "draw");

    Module* const m = ctx.create_module();
    Block*        bb = m->body()->first_block();
    if (bb == nullptr)
    {
        bb = ctx.create_block(0U);
        m->body()->append(bb);
    }

    // ── the color TARGET: a placeholder Image + a color_attachment over it. The device target is resolved at RECORD (the
    // target resolver maps this attachment op → RecordContext.color_target(pass_param_id("color"))); the `source` attr pins
    // that identity (advisor constraint #2). `load` selects LoadOp (a WBOIT composite LOADS; the ordinary case CLEARS).
    Operation* const timg = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
    bb->append(timg);
    Value*           imgv[1] = {timg->result(0U)};
    Operation* const col =
        ctx.create_operation(k_col, containers::ConstSpan<Value*>(imgv, 1U), 1U,
                             render::type_color_attachment(ctx, timg->result(0U)->type()));
    ctx.set_attr(col, "load", ctx.attr_string(desc.load ? containers::StringView("load") : containers::StringView("clear")));
    bb->append(col);

    // ── the SCOPE. width/height are a 1x1 PLACEHOLDER: find_render_misuse requires >= 1, but a fullscreen pass's real
    // render area is the RESOLVED TARGET's size, applied at record (CEIR-16-3c materialize-from-target), not a build-time
    // constant. sample_count defaults to 1 (a fullscreen pass is single-sampled).
    Value*           atts[1] = {col->result(0U)};
    Operation* const scope   = ctx.create_operation(k_scope, containers::ConstSpan<Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(scope, "width", ctx.attr_int(1));
    ctx.set_attr(scope, "height", ctx.attr_int(1));
    // ⭐ CEIR-16-3c-4: the render area is the RESOLVED colour target's size at record, NOT the 1x1 placeholder above (a
    // fullscreen composite binds to a per-cascade atlas / half-res buffer / swapchain of unknown-at-build size).
    ctx.set_attr(scope, "extent_from_target", ctx.attr_bool(true));
    bb->append(scope);
    Block* const rb = ctx.create_block(0U);
    scope->region(0)->append(rb);

    // ── the procedural 3-vertex, 1-instance draw (GeometryKind::None — the VS reads gl_VertexIndex). Counts are Index-typed
    // arith.const operands (find_render_misuse: DrawCountNotIndex).
    Operation* const vc = ctx.create_operation(k_cst, {}, 1U, ctx.type_index());
    ctx.set_attr(vc, "value", ctx.attr_int(3));
    rb->append(vc);
    Operation* const ic = ctx.create_operation(k_cst, {}, 1U, ctx.type_index());
    ctx.set_attr(ic, "value", ctx.attr_int(1));
    rb->append(ic);

    // ── the binding tail. The SHAPE selects the binding declarations — each a resource.declare whose CEIR-3c TYPE picks the
    // materialized kind (Image → SampledTexture, Sampler → Comparison/plain Sampler; CEIR-16-2/3a). `slot` is the descriptor
    // register, `source` the RecordContext identity (advisor #2). `access` is one `r` token per binding (find_render_misuse:
    // DrawAccessArity token-count == num bindings). `nb` counts bindings EMITTED — NOT desc.num_inputs (the atlas emits 2 for 1).
    Value*   ops[2U + 8U + 1U]; // counts + up to 8 texture inputs + a possible companion sampler (the atlas)
    ops[0]        = vc->result(0U);
    ops[1]        = ic->result(0U);
    char       accbuf[32];
    crd::u32   alen        = 0U;
    crd::u32   nb          = 0U;
    const auto push_access = [&accbuf, &alen, &nb]() {
        if (nb > 0U) { accbuf[alen++] = ','; }
        accbuf[alen++] = 'r';
    };

    const bool atlas = (desc.num_inputs == 1U) && desc.inputs[0].is_depth && !desc.depth_as_float;
    // The BINDLESS shape: >1 read (a TAA/composite fan) OR a single blend-LOAD read (the WBOIT resolve, record_fullscreen_raster's
    // `loads && blend != Opaque` arm). The atlas takes priority (a depth-shadow read is never blend-loaded).
    const bool bindless = !atlas && ((desc.num_inputs > 1U)
                                     || (desc.num_inputs == 1U && desc.load && desc.blend != crd::gpu::BlendMode::Opaque));
    if (atlas)
    {
        // ⭐ CEIR-16-3b-2: the SHADOW-ATLAS shape — a single depth read binds as the atlas TEXTURE @ slot 4 + a COMPARISON
        // sampler @ slot 5 (bind_atlas parity; ⛔⛔⛔ REN-40-D: the encoder recognises a shadow atlas by SLOT 4, and a
        // comparison sampler where the shader declared a plain one — or vice-versa — renders every moment shadow black).
        Operation* const atex = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        ctx.set_attr(atex, "slot", ctx.attr_int(4));
        ctx.set_attr(atex, "source", ctx.attr_int(static_cast<crd::i64>(desc.inputs[0].source_param)));
        rb->append(atex);
        push_access();
        ops[2U + nb++] = atex->result(0U);
        Operation* const asamp = ctx.create_operation(k_decl, {}, 1U, ctx.type_sampler(/*comparison*/ true));
        ctx.set_attr(asamp, "slot", ctx.attr_int(5));
        rb->append(asamp);
        push_access();
        ops[2U + nb++] = asamp->result(0U);
    }
    else if (bindless)
    {
        // ⭐ CEIR-16-3b-3: the BINDLESS shape — the reads bind as ONE resource_table(image) → a BindlessTextureArray
        // (16b-3a-1). The N-ness lives BEHIND the record resolver (it gathers input0..N, ABORT-on-gap per the resolve-or-abort
        // scar); the IR carries ONE binding whose `source` names the FIRST input (input0), the gather anchor.
        Operation* const btbl =
            ctx.create_operation(k_decl, {}, 1U, ctx.type_resource_table(ctx.type_image(ImageDim::Dim2D, ctx.type_f32())));
        ctx.set_attr(btbl, "slot", ctx.attr_int(0));
        ctx.set_attr(btbl, "source", ctx.attr_int(static_cast<crd::i64>(desc.inputs[0].source_param)));
        rb->append(btbl);
        push_access();
        ops[2U + nb++] = btbl->result(0U);
        // the OPTIONAL constants buffer (the TAA-resolve shape) — a StorageBuffer at slot 0 (Object frequency, its OWN set,
        // so slot 0 does not collide with the bindless array's Material slot 0).
        if (desc.constants_param != 0U)
        {
            Operation* const cbuf = ctx.create_operation(k_decl, {}, 1U, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
            ctx.set_attr(cbuf, "slot", ctx.attr_int(0));
            ctx.set_attr(cbuf, "source", ctx.attr_int(static_cast<crd::i64>(desc.constants_param)));
            rb->append(cbuf);
            push_access();
            ops[2U + nb++] = cbuf->result(0U);
        }
    }
    else
    {
        // the PLAIN shape: each declared read → an Image binding at its ordinal slot.
        for (crd::u32 i = 0; i < desc.num_inputs; ++i)
        {
            Operation* const bimg = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
            ctx.set_attr(bimg, "slot", ctx.attr_int(static_cast<crd::i64>(i)));
            ctx.set_attr(bimg, "source", ctx.attr_int(static_cast<crd::i64>(desc.inputs[i].source_param)));
            rb->append(bimg);
            push_access();
            ops[2U + nb++] = bimg->result(0U);
        }
    }
    Operation* const draw = ctx.create_operation(k_draw, containers::ConstSpan<Value*>(ops, 2U + nb), 0U);
    // ⛔ the @program symbol is a PLACEHOLDER (find_render_misuse: ProgramNotSymbol requires a Symbol) — the ACTUAL raster
    // program is the pass's, resolved at record from RecordContext.programs().raster, NOT this identity.
    ctx.set_attr(draw, "program", ctx.attr_symbol(containers::StringView("fullscreen")));
    ctx.set_attr(draw, "access", ctx.attr_string(containers::StringView(accbuf, alen)));
    // ⭐ CEIR-16-3b-1b: a fullscreen draw is PROCEDURAL (the VS reads gl_VertexIndex) EVEN when it binds textures — pin it, or
    // the >2-operands heuristic infers StoragePull and the encoder's vertex-pull arm (no vertex buffer) draws nothing.
    ctx.set_attr(draw, "geometry", ctx.attr_string(containers::StringView("procedural")));
    rb->append(draw);

    // ── verify (the verifier-first contract execute_render_lowered assumes) + lower.
    if (ctx.find_render_misuse(*m).kind != RenderMisuseKind::None) { return false; }
    lower_region(ctx, *bb, out_plan);
    return true;
}

bool build_mesh_indirect_ceir(Context& ctx, const MeshIndirectBuildDesc& desc, containers::Array<LoweredCommand>& out_plan)
{
    out_plan.clear();
    (void)arith::register_arith_ops(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)render::register_dialect(ctx);

    const OpId k_decl  = ctx.intern_op("resource", "declare");
    const OpId k_col   = ctx.intern_op("render", "color_attachment");
    const OpId k_scope = ctx.intern_op("render", "scope");
    const OpId k_mind  = ctx.intern_op("render", "mesh_dispatch_indirect");

    Module* const m  = ctx.create_module();
    Block*        bb = m->body()->first_block();
    if (bb == nullptr)
    {
        bb = ctx.create_block(0U);
        m->body()->append(bb);
    }

    // ── the color TARGET (record_mesh_indirect always CLEARS) + a color_attachment carrying the clear colour. ⛔ Unlike the
    // fullscreen composite (whose full-screen triangle overwrites every pixel, so the clear colour never showed), a mesh
    // dispatch can leave uncovered pixels — so the clear colour MUST be carried (clear_r/g/b/a, the materializer's attrs).
    Operation* const timg = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
    bb->append(timg);
    Value*           imgv[1] = {timg->result(0U)};
    Operation* const col =
        ctx.create_operation(k_col, containers::ConstSpan<Value*>(imgv, 1U), 1U,
                             render::type_color_attachment(ctx, timg->result(0U)->type()));
    ctx.set_attr(col, "load", ctx.attr_string(containers::StringView("clear")));
    ctx.set_attr(col, "clear_r", ctx.attr_float(desc.clear.r));
    ctx.set_attr(col, "clear_g", ctx.attr_float(desc.clear.g));
    ctx.set_attr(col, "clear_b", ctx.attr_float(desc.clear.b));
    ctx.set_attr(col, "clear_a", ctx.attr_float(desc.clear.a));
    bb->append(col);

    // ── the SCOPE (1x1 placeholder + extent_from_target: the dispatch renders the full resolved colour target). ──
    Value*           atts[1] = {col->result(0U)};
    Operation* const scope   = ctx.create_operation(k_scope, containers::ConstSpan<Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(scope, "width", ctx.attr_int(1));
    ctx.set_attr(scope, "height", ctx.attr_int(1));
    ctx.set_attr(scope, "extent_from_target", ctx.attr_bool(true));
    bb->append(scope);
    Block* const rb = ctx.create_block(0U);
    scope->region(0)->append(rb);

    // ── the %args buffer (operand 0 of mesh_dispatch_indirect) — a Buffer-typed declare the RECORD resolver maps to the
    // pass's `args` storage (source attr = args_param). find_render_misuse: IndirectArgsNotBuffer requires a Buffer/View. ──
    Operation* const argsd = ctx.create_operation(k_decl, {}, 1U, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
    ctx.set_attr(argsd, "source", ctx.attr_int(static_cast<crd::i64>(desc.args_param)));
    rb->append(argsd);

    // ── mesh_dispatch_indirect(%args) {program=@mesh, access="", args_offset}. NO descriptor bindings (record_mesh_indirect
    // binds none) ⇒ access arity == num_operands-1 == 0. The @program is a PLACEHOLDER (the pass's raster program resolves at
    // record); args_offset is a plain attr the materializer folds onto GeometrySource.args_offset. ──
    Value*           ops[1] = {argsd->result(0U)};
    Operation* const draw   = ctx.create_operation(k_mind, containers::ConstSpan<Value*>(ops, 1U), 0U);
    ctx.set_attr(draw, "program", ctx.attr_symbol(containers::StringView("mesh")));
    ctx.set_attr(draw, "access", ctx.attr_string(containers::StringView("")));
    ctx.set_attr(draw, "args_offset", ctx.attr_int(static_cast<crd::i64>(desc.args_offset)));
    rb->append(draw);

    if (ctx.find_render_misuse(*m).kind != RenderMisuseKind::None) { return false; }
    lower_region(ctx, *bb, out_plan);
    return true;
}

bool build_amplify_ceir(Context& ctx, const AmplifyBuildDesc& desc, containers::Array<LoweredCommand>& out_plan)
{
    out_plan.clear();
    (void)arith::register_arith_ops(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)render::register_dialect(ctx);

    const OpId k_decl  = ctx.intern_op("resource", "declare");
    const OpId k_col   = ctx.intern_op("render", "color_attachment");
    const OpId k_scope = ctx.intern_op("render", "scope");
    const OpId k_list  = ctx.intern_op("render", "mesh_dispatch_list");

    Module* const m  = ctx.create_module();
    Block*        bb = m->body()->first_block();
    if (bb == nullptr)
    {
        bb = ctx.create_block(0U);
        m->body()->append(bb);
    }

    // ── the color TARGET (record_amplify_raster always CLEARS) + a color_attachment carrying the clear colour (a mesh/patch
    // dispatch can leave uncovered pixels — carry the real clear, same as build_mesh_indirect_ceir). ──
    Operation* const timg = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
    bb->append(timg);
    Value*           imgv[1] = {timg->result(0U)};
    Operation* const col =
        ctx.create_operation(k_col, containers::ConstSpan<Value*>(imgv, 1U), 1U,
                             render::type_color_attachment(ctx, timg->result(0U)->type()));
    ctx.set_attr(col, "load", ctx.attr_string(containers::StringView("clear")));
    ctx.set_attr(col, "clear_r", ctx.attr_float(desc.clear.r));
    ctx.set_attr(col, "clear_g", ctx.attr_float(desc.clear.g));
    ctx.set_attr(col, "clear_b", ctx.attr_float(desc.clear.b));
    ctx.set_attr(col, "clear_a", ctx.attr_float(desc.clear.a));
    bb->append(col);

    // ── the SCOPE (1x1 placeholder + extent_from_target). ──
    Value*           atts[1] = {col->result(0U)};
    Operation* const scope   = ctx.create_operation(k_scope, containers::ConstSpan<Value*>(atts, 1U), 0U, {}, 1U);
    ctx.set_attr(scope, "width", ctx.attr_int(1));
    ctx.set_attr(scope, "height", ctx.attr_int(1));
    ctx.set_attr(scope, "extent_from_target", ctx.attr_bool(true));
    bb->append(scope);
    Block* const rb = ctx.create_block(0U);
    scope->region(0)->append(rb);

    // ── mesh_dispatch_list() {program=@amp, primitive, fallback_count, access=""}. ZERO operands — the record-time walk
    // expands it over the host DrawList (the per-item program/count/storage). @program is a PLACEHOLDER (resolved at record).
    Operation* const draw = ctx.create_operation(k_list, {}, 0U);
    ctx.set_attr(draw, "program", ctx.attr_symbol(containers::StringView("amp")));
    ctx.set_attr(draw, "primitive",
                 ctx.attr_string(desc.patches ? containers::StringView("patches") : containers::StringView("meshlet")));
    ctx.set_attr(draw, "fallback_count", ctx.attr_int(static_cast<crd::i64>(desc.fallback_count)));
    ctx.set_attr(draw, "access", ctx.attr_string(containers::StringView("")));
    rb->append(draw);

    if (ctx.find_render_misuse(*m).kind != RenderMisuseKind::None) { return false; }
    lower_region(ctx, *bb, out_plan);
    return true;
}

namespace
{
// The render.depth_attachment `compare` string — a 1:1 with the gpu DepthCompare enum (materialize_rendering_desc maps it
// back). ⛔ the scene is REVERSE-Z, so the live pass authors `greater_equal`; a wrong mapping fails every depth test.
containers::StringView depth_compare_str(crd::gpu::DepthCompare c)
{
    switch (c)
    {
    case crd::gpu::DepthCompare::Never:        return containers::StringView("never");
    case crd::gpu::DepthCompare::Less:         return containers::StringView("less");
    case crd::gpu::DepthCompare::Equal:        return containers::StringView("equal");
    case crd::gpu::DepthCompare::LessEqual:    return containers::StringView("less_equal");
    case crd::gpu::DepthCompare::Greater:      return containers::StringView("greater");
    case crd::gpu::DepthCompare::NotEqual:     return containers::StringView("not_equal");
    case crd::gpu::DepthCompare::GreaterEqual: return containers::StringView("greater_equal");
    case crd::gpu::DepthCompare::Always:       return containers::StringView("always");
    }
    return containers::StringView("less_equal");
}
} // namespace

bool build_scene_ceir(Context& ctx, const SceneBuildDesc& desc, containers::Array<LoweredCommand>& out_plan)
{
    out_plan.clear();
    if (!desc.has_color && !desc.has_depth) { return false; } // a render.scope needs at least one attachment

    (void)arith::register_arith_ops(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)render::register_dialect(ctx);

    const OpId k_decl  = ctx.intern_op("resource", "declare");
    const OpId k_col   = ctx.intern_op("render", "color_attachment");
    const OpId k_dep   = ctx.intern_op("render", "depth_attachment");
    const OpId k_scope = ctx.intern_op("render", "scope");
    const OpId k_list  = ctx.intern_op("render", "scene_draw_list");

    Module* const m  = ctx.create_module();
    Block*        bb = m->body()->first_block();
    if (bb == nullptr)
    {
        bb = ctx.create_block(0U);
        m->body()->append(bb);
    }

    Value*   atts[2];
    crd::u32 natt = 0U;

    // ── the OPTIONAL colour attachment. load=true STACKS (LoadOp::Load — a `load` pass never clears); else CLEAR carrying
    // the clear colour (a scene draw can leave pixels uncovered, so the clear MUST ride, like the mesh/amplify builders). The
    // device target resolves at record via the target resolver keyed on the OP KIND (colour_attachment → the colour slot). ──
    if (desc.has_color)
    {
        Operation* const timg = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        bb->append(timg);
        Value*           imgv[1] = {timg->result(0U)};
        Operation* const col =
            ctx.create_operation(k_col, containers::ConstSpan<Value*>(imgv, 1U), 1U,
                                 render::type_color_attachment(ctx, timg->result(0U)->type()));
        ctx.set_attr(col, "load",
                     ctx.attr_string(desc.load ? containers::StringView("load") : containers::StringView("clear")));
        ctx.set_attr(col, "clear_r", ctx.attr_float(desc.clear.r));
        ctx.set_attr(col, "clear_g", ctx.attr_float(desc.clear.g));
        ctx.set_attr(col, "clear_b", ctx.attr_float(desc.clear.b));
        ctx.set_attr(col, "clear_a", ctx.attr_float(desc.clear.a));
        bb->append(col);
        atts[natt++] = col->result(0U);
    }
    // ── the OPTIONAL depth attachment (a forward pass's depth-test, or the SOLE attachment of a depth-only shadow cascade —
    // a 0-COLOUR scope). load_depth STACKS depth (a depth-prepass consumer); compare is the reverse-Z DepthCompare. The device
    // depth target resolves at record via the target resolver keyed on the OP KIND (depth_attachment → the depth slot / the
    // colour target's bundled depth / the shared_depth image — the 3 record-time modes live in the resolver, not here). ──
    if (desc.has_depth)
    {
        Operation* const dimg = ctx.create_operation(k_decl, {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32()));
        bb->append(dimg);
        Value*           dv[1] = {dimg->result(0U)};
        Operation* const dep =
            ctx.create_operation(k_dep, containers::ConstSpan<Value*>(dv, 1U), 1U,
                                 render::type_depth_attachment(ctx, dimg->result(0U)->type()));
        ctx.set_attr(dep, "load",
                     ctx.attr_string(desc.load_depth ? containers::StringView("load") : containers::StringView("clear")));
        ctx.set_attr(dep, "clear_depth", ctx.attr_float(desc.clear_depth));
        ctx.set_attr(dep, "compare", ctx.attr_string(depth_compare_str(desc.depth_compare)));
        bb->append(dep);
        atts[natt++] = dep->result(0U);
    }

    // ── the SCOPE (1x1 placeholder + extent_from_target: the scene renders the full resolved target). ──
    Operation* const scope = ctx.create_operation(k_scope, containers::ConstSpan<Value*>(atts, natt), 0U, {}, 1U);
    ctx.set_attr(scope, "width", ctx.attr_int(1));
    ctx.set_attr(scope, "height", ctx.attr_int(1));
    ctx.set_attr(scope, "extent_from_target", ctx.attr_bool(true));
    bb->append(scope);
    Block* const rb = ctx.create_block(0U);
    scope->region(0)->append(rb);

    // ── scene_draw_list() {program=@scene, access=""}. ZERO operands — the record-time walk expands it over the host DrawList
    // (per-item program/storage/texture/args/counts). @program is a PLACEHOLDER (resolved at record from the pass's raster). ──
    Operation* const draw = ctx.create_operation(k_list, {}, 0U);
    ctx.set_attr(draw, "program", ctx.attr_symbol(containers::StringView("scene")));
    ctx.set_attr(draw, "access", ctx.attr_string(containers::StringView("")));
    rb->append(draw);

    if (ctx.find_render_misuse(*m).kind != RenderMisuseKind::None) { return false; }
    lower_region(ctx, *bb, out_plan);
    return true;
}
} // namespace crd::ceir::gpu
