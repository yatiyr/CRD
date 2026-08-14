// CEIR-15a (ADR-0127) — the FrameGraphDesc -> ceir.frame FORWARD converter (§126 step 1/2). A FrameGraphBuilder-built desc
// converts to a `ceir.frame` module (func main { frame.graph { resource.declare* frame.draw_list* frame.pass* } }) that is
// Context::find_frame_misuse-CLEAN and round-trips through CEIR text (print -> parse -> print fixpoint). The backward
// converter + the desc==desc' round-trip-identity gate are CEIR-15a-3b.

#include <crd/framecook/frame_asset.hpp>
#include <crd/framecook/frame_ceir.hpp>

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/frame.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/gpu/frame_graph.hpp> // FgImageFormat

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;             // NOLINT(google-build-using-namespace)
using namespace crd::framecook;  // NOLINT(google-build-using-namespace)
using crd::containers::String;
using crd::containers::StringView;

namespace
{
// StringView has no find(); a small substring test (the module text contains a frame.history op iff the routing fired).
[[nodiscard]] bool sv_contains(StringView hay, StringView needle)
{
    if (needle.size() == 0U) { return true; }
    if (hay.size() < needle.size()) { return false; }
    for (crd::usize i = 0; i + needle.size() <= hay.size(); ++i)
    {
        if (StringView(hay.data() + i, needle.size()) == needle) { return true; }
    }
    return false;
}
// The (flat) frame.graph region's first block, walking module -> func -> frame.graph (the converter builds one of each).
[[nodiscard]] ceir::Block* graph_block(ceir::Context& ctx, ceir::Module& m)
{
    for (ceir::Block* tb = m.body()->first_block(); tb != nullptr; tb = tb->next_in_region())
    {
        for (ceir::Operation* fn = tb->first_op(); fn != nullptr; fn = fn->next_in_block())
        {
            for (crd::u32 fr = 0; fr < fn->num_regions(); ++fr)
            {
                for (ceir::Block* fbk = fn->region(fr)->first_block(); fbk != nullptr; fbk = fbk->next_in_region())
                {
                    for (ceir::Operation* g = fbk->first_op(); g != nullptr; g = g->next_in_block())
                    {
                        if (ctx.op_name(g->kind()) == StringView("frame.graph") && g->num_regions() > 0U)
                        {
                            return g->region(0)->first_block();
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}
// The op of kind `op_kind` with the given `name` symbol attr in the (flat) frame.graph — mutating a found op's attrs is how
// the tests INJECT a defect the forward converter never emits (an rw access, a frame_kind/lifetime desync, a bad dimension).
[[nodiscard]] ceir::Operation* find_graph_op(ceir::Context& ctx, ceir::Module& m, StringView op_kind, StringView name)
{
    ceir::Block* const gb = graph_block(ctx, m);
    for (ceir::Operation* op = gb != nullptr ? gb->first_op() : nullptr; op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) != op_kind) { continue; }
        const ceir::AttrId a = op->attr(StringView("name"));
        if (a.valid() && ctx.attr_value(a).s == name) { return op; }
    }
    return nullptr;
}
[[nodiscard]] const FramePassDesc* find_pass_desc(const FrameGraphDesc& d, StringView name)
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(d.passes.size()); ++i)
    {
        if (StringView(d.passes[i].name.data(), d.passes[i].name.size()) == name) { return &d.passes[i]; }
    }
    return nullptr;
}
[[nodiscard]] bool ref_list_contains(const crd::containers::Array<FrameResourceRef>& refs, StringView name)
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(refs.size()); ++i)
    {
        if (StringView(refs[i].name.data(), refs[i].name.size()) == name) { return true; }
    }
    return false;
}
// A small forward-and-back-through-text scene graph: a scaled scene image, an opaque draw list, a geometry pass that
// writes the scene (iterating the draw list), and a present pass that reads it.
void build_scene(FrameGraphBuilder& fb)
{
    fb.add_scaled_image("scene", gpu::FgImageFormat::RGBA8Unorm, 1.0F, /*sampled=*/true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    // geometry pass renders into the scene target; the forward pass reads it and writes the swapchain (@output). Writing
    // @output is what makes the graph VALID (a frame must produce the output endpoint); a raster pass can write it directly.
    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_writes(geo, "scene");
    const crd::u32 fwd = fb.add_pass("forward", "raster.geometry");
    fb.pass_draw_list(fwd, "opaque");
    fb.pass_reads(fwd, "scene");
    fb.pass_writes(fwd, "@output");
}

// CEIR-15c-0 (Fork B, B1): a TAA-history graph — a ping-pong resource read as the PREVIOUS frame + written as THIS frame.
// The taa pass READS its own history (prev), so the forward converter routes that read through a `frame.history` op (a
// distinct resource_root) so the 15d hazard walk never derives a false intra-frame RAW between the prev-read and curr-write.
void build_taa(FrameGraphBuilder& fb, memory::IAllocator* a)
{
    fb.add_scaled_image("scene", gpu::FgImageFormat::RGBA8Unorm, 1.0F, /*sampled=*/true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);

    // a ping-pong TAA-history image. The builder only makes transients, so push the PingPong kind directly (mirroring
    // add_IMAGE's field pattern — an ABSOLUTE width/height, NOT scale: a persistent/ping-pong key must be stable across
    // frames, so scale-only sizing is PersistentNeedsSize). It must be BOTH read and written for the pair to rotate.
    FrameResourceDesc h(a);
    h.name.append("history", 7);
    h.kind    = FrameResourceKind::PingPongImage;
    h.format  = gpu::FgImageFormat::RGBA8Unorm;
    h.width   = 1920U;
    h.height  = 1080U;
    h.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(h));

    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_writes(geo, "scene");
    // the TAA resolve: reads this frame's scene + the PREVIOUS history frame, writes the NEW history + the swapchain. A
    // geometry pass needs a draw list (MissingDrawList otherwise) — it samples history while it draws.
    const crd::u32 taa = fb.add_pass("taa", "raster.geometry");
    fb.pass_draw_list(taa, "opaque");
    fb.pass_reads(taa, "scene");
    fb.pass_reads(taa, "history");
    fb.pass_writes(taa, "history");
    fb.pass_writes(taa, "@output");
}

// build_scene MINUS the forward pass: the geometry pass writes only the transient `scene`, so nothing writes @output.
void build_no_output(FrameGraphBuilder& fb)
{
    fb.add_scaled_image("scene", gpu::FgImageFormat::RGBA8Unorm, 1.0F, /*sampled=*/true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_writes(geo, "scene"); // a transient — never @output
}

// A ping-pong (history) resource that is WRITTEN but never READ — the pair can never rotate (PingPongNeedsBothWays). Like
// build_taa, but the resolve pass drops the `pass_reads(taa, "history")` (the frame.history prev-frame read).
void build_pingpong_writeonly(FrameGraphBuilder& fb, memory::IAllocator* a)
{
    fb.add_scaled_image("scene", gpu::FgImageFormat::RGBA8Unorm, 1.0F, /*sampled=*/true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    FrameResourceDesc h(a); // an absolute-sized ping-pong (dodges PersistentNeedsSize)
    h.name.append("history", 7);
    h.kind    = FrameResourceKind::PingPongImage;
    h.format  = gpu::FgImageFormat::RGBA8Unorm;
    h.width   = 1920U;
    h.height  = 1080U;
    h.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(h));
    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_writes(geo, "scene");
    const crd::u32 taa = fb.add_pass("taa", "raster.geometry");
    fb.pass_draw_list(taa, "opaque");
    fb.pass_reads(taa, "scene");
    fb.pass_writes(taa, "history"); // written this frame, but NEVER read — the pair cannot rotate
    fb.pass_writes(taa, "@output");
}

// The desc-side DIFFERENTIAL ORACLE: the full cook pipeline (emit -> parse -> validate). parse_frame_toml owns the
// per-category DuplicateName; validate_frame_graph owns NoOutputPass. validate_ceir_frame must agree with this verdict.
[[nodiscard]] FrameCookError cook_verdict(const FrameGraphDesc& d, memory::IAllocator* a)
{
    const String   toml = emit_frame_toml(d, a);
    FrameGraphDesc d2(a);
    const FrameCookError pe = parse_frame_toml(StringView(toml.data(), toml.size()), d2);
    if (pe != FrameCookError::Ok) { return pe; }
    return validate_frame_graph(d2);
}

// ── CEIR-15e: a COMPOSED graph (include + anchor + inject) — the ONLY input `to_ceir_frame` must NOT accept directly.
// The parent includes a `grade` subgraph, declares an anchor after `forward`, and injects `extra` there. The live
// scene-renderer resolver parses each subgraph FRESH (never a re-flattened cache), so `resolve_composed_sub` mirrors it:
// the child's anchors are live only DURING expansion, which is what makes flatten's end-of-pass anchor clear safe. ──
constexpr const char* kComposedSub = R"(
schema = 1
name   = "grade"

[[pass]]
name   = "tone"
kind   = "raster.fullscreen"
shader = "crd://shaders/tone"
reads  = ["@input"]
writes = ["@output"]
)";
constexpr const char* kComposedParent = R"(
schema = 1
name   = "composed_ceir"

[[include]]
graph = "crd://technique/grade"
as    = "g"
bind  = { "@input" = "graded", "@output" = "@output" }

[[anchor]]
name  = "after_scene"
after = ["forward"]

[[resource]]
name    = "scene_color"
format  = "RGBA16F"
scale   = 1.0
sampled = true

[[resource]]
name    = "graded"
format  = "RGBA16F"
scale   = 1.0
sampled = true

[[pass]]
name      = "forward"
kind      = "raster.geometry"
draw_list = "visible"
writes    = ["scene_color"]

[[pass]]
name   = "extra"
kind   = "raster.fullscreen"
shader = "crd://shaders/extra"
reads  = ["scene_color"]
writes = ["graded"]

[[inject]]
at   = "after_scene"
pass = "extra"

[[draw_list]]
name = "visible"
all  = ["MeshRenderer", "Transform"]
)";
const FrameGraphDesc* g_composed_sub = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
const FrameGraphDesc* resolve_composed_sub(StringView name, void* /*user*/)
{
    return name == StringView("crd://technique/grade") ? g_composed_sub : nullptr;
}
} // namespace

TEST_CASE("ceir 15a: a FrameGraphDesc converts to a find_frame_misuse-clean ceir.frame", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_frame");
    build_scene(fb);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // the converted frame is well-formed
}

TEST_CASE("ceir 15a: a converted ceir.frame round-trips through CEIR text (print == parse-print)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_frame");
    build_scene(fb);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const String txt = ceir::print(ctx, *m, &alloc);

    ceir::Context ctx2(&alloc);
    (void)ceir::arith::register_arith_ops(ctx2);
    (void)ceir::func::register_dialect(ctx2);
    (void)ceir::resource::register_resource_ops(ctx2);
    (void)ceir::frame::register_dialect(ctx2);
    const ceir::ParseResult pr = ceir::parse(ctx2, StringView(txt.data(), txt.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String txt2 = ceir::print(ctx2, *pr.module, &alloc);
    CHECK(StringView(txt.data(), txt.size()) == StringView(txt2.data(), txt2.size())); // print(parse(print)) fixpoint
    CHECK(ctx2.find_frame_misuse(*pr.module).kind == ceir::FrameMisuseKind::None);      // and it re-verifies clean
}

// CEIR-15a-3b: THE ROUND-TRIP-IDENTITY GATE — desc -> ceir.frame -> desc' is emit_frame_toml-IDENTICAL. This is the
// semantic-equality instrument (FrameGraphDesc has no operator==; the emitted TOML is its canonical form, the §121 render
// precedent). It proves the FORWARD + BACKWARD converters are LOSSLESS: every field emit_frame_toml serializes survives.
TEST_CASE("ceir 15a: the FrameGraphDesc<->ceir.frame round-trip is emit_frame_toml-identical", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_frame");
    build_scene(fb);
    const String toml_a = emit_frame_toml(fb.desc(), &alloc);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc2)); // reconstruct the desc from the ceir.frame module
    const String toml_b = emit_frame_toml(desc2, &alloc);

    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size())); // desc == to->from(desc)
}

// CEIR-15b (§126 step 2): the C++ FrameGraphBuilder frontend AND the .frame.toml TEXT frontend END at the SAME `ceir.frame`.
// Both produce a FrameGraphDesc; to_ceir_frame is deterministic, so a builder-built graph and the SAME graph parsed from its
// emitted TOML lower to BYTE-IDENTICAL ceir.frame. This is the "one runtime/compiler architecture" contract (§39): there is
// no privileged frontend — the CEIR representation is canonical regardless of how the graph was authored.
TEST_CASE("ceir 15b: FrameGraphBuilder and .frame.toml end at byte-identical ceir.frame", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_frame");
    build_scene(fb);

    // Path 1: the C++ BUILDER frontend -> desc -> ceir.frame.
    ceir::Context       ctx1(&alloc);
    ceir::Module* const m1 = to_ceir_frame(fb.desc(), ctx1);
    REQUIRE(m1 != nullptr);
    const String ceir1 = ceir::print(ctx1, *m1, &alloc);

    // Path 2: the .frame.toml TEXT frontend (emit the builder's desc, re-parse it) -> desc -> ceir.frame.
    const String   toml = emit_frame_toml(fb.desc(), &alloc);
    FrameGraphDesc desc_toml(&alloc);
    REQUIRE(parse_frame_toml(StringView(toml.data(), toml.size()), desc_toml) == FrameCookError::Ok);
    ceir::Context       ctx2(&alloc);
    ceir::Module* const m2 = to_ceir_frame(desc_toml, ctx2);
    REQUIRE(m2 != nullptr);
    const String ceir2 = ceir::print(ctx2, *m2, &alloc);

    CHECK(StringView(ceir1.data(), ceir1.size()) == StringView(ceir2.data(), ceir2.size())); // §126: one canonical CEIR
}

// CEIR-15e (§159): the COMPOSITION contract, pinned end to end — the failure the live RAF-10 flag-ON render exposed. The
// pipeline is `parse -> flatten_frame_graph -> to_ceir_frame`: composition (includes/anchors/injects) is frontend-only and
// has no execution semantics, so (1) an UN-flattened composed desc MUST be rejected by the converter, and (2) the FLATTENED
// desc MUST be anchor-free — flatten consumes the spent scaffolding — and the converter accepts it and round-trips it
// emit_frame_toml-losslessly. Before flatten cleared the consumed anchors, (2) failed: a residual `after_scene` anchor
// tripped the converter's composition guard, so the shipping renderer could not render an app-COMPOSED graph through the
// CEIR path (RAF-10's `app_custom`), while `app_authored` (no composition) went through. This gate is the device-free pin
// of that whole path; the on-device RAF-10 flag-ON render additionally proves the injected CUSTOM executor round-trips.
TEST_CASE("ceir 15e: a composed graph flattens to an anchor-free ordinary graph the converter round-trips",
          "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;

    FrameGraphDesc sub(&alloc);
    REQUIRE(parse_frame_toml(StringView(kComposedSub), sub) == FrameCookError::Ok);
    g_composed_sub = &sub;

    FrameGraphDesc parent(&alloc);
    String         where(&alloc);
    REQUIRE(parse_frame_toml(StringView(kComposedParent), parent, &where) == FrameCookError::Ok);
    REQUIRE(parent.includes.size() == 1U);
    REQUIRE(parent.anchors.size() == 1U);
    REQUIRE(parent.injects.size() == 1U);

    // (1) THE GUARD: an un-flattened composed desc carries composition metadata the CEIR dialect cannot model — REJECTED.
    ceir::Context ctx_bad(&alloc);
    CHECK(to_ceir_frame(parent, ctx_bad) == nullptr);

    // flatten expands the include into plain passes (`g.tone`), positions the inject (`extra` after `forward`), and
    // CONSUMES the anchor. The output is an ORDINARY graph — no residual composition metadata of any kind.
    FrameGraphDesc flat(&alloc);
    REQUIRE(flatten_frame_graph(parent, &resolve_composed_sub, nullptr, flat, &where) == FrameCookError::Ok);
    CHECK(flat.includes.size() == 0U);
    CHECK(flat.injects.size() == 0U);
    CHECK(flat.anchors.size() == 0U); // ⛔ the fix: spent scaffolding dropped, so the converter's guard is satisfied
    REQUIRE(find_pass_desc(flat, StringView("forward")) != nullptr);
    REQUIRE(find_pass_desc(flat, StringView("extra")) != nullptr);
    REQUIRE(find_pass_desc(flat, StringView("g.tone")) != nullptr);

    // (2) the flattened ORDINARY graph converts, and the forward+backward round-trip is emit_frame_toml-lossless.
    const String        toml_a = emit_frame_toml(flat, &alloc);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(flat, ctx);
    REQUIRE(m != nullptr);
    FrameGraphDesc flat2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, flat2));
    const String toml_b = emit_frame_toml(flat2, &alloc);
    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size()));

    g_composed_sub = nullptr;
}

// CEIR-15c-0 (Fork B, B1): the frame.history ROUTING + round-trip LOCK. A ping-pong (TAA-history) resource that a pass reads
// as the previous frame must (forward) route that read through a `frame.history` op — so its result is a DISTINCT
// resource_root and the 15d hazard walk never derives a false intra-frame RAW vs the same-frame write. The backward converter
// recovers the read name from the wrapped declare, so the FrameGraphDesc round-trips emit_frame_toml-identically.
TEST_CASE("ceir 15c: a ping-pong history read routes through frame.history and round-trips", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_taa");
    build_taa(fb, &alloc);
    REQUIRE(fb.validate() == FrameCookError::Ok); // a round-trip lock on a cook-INVALID graph is a weak lock — this one is valid
    const String toml_a = emit_frame_toml(fb.desc(), &alloc);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // the frame.history operand is a history declare

    // the routing actually happened — a frame.history op is present (a plain read would omit it, defeating the Fork-B fix).
    const String txt = ceir::print(ctx, *m, &alloc);
    CHECK(sv_contains(StringView(txt.data(), txt.size()), StringView("frame.history")));

    // frame.history must survive the CEIR TEXT parser too (print -> parse -> print fixpoint + re-verify) — the op had never
    // been round-tripped through text before this slice.
    ceir::Context ctx2(&alloc);
    (void)ceir::arith::register_arith_ops(ctx2);
    (void)ceir::func::register_dialect(ctx2);
    (void)ceir::resource::register_resource_ops(ctx2);
    (void)ceir::frame::register_dialect(ctx2);
    const ceir::ParseResult pr = ceir::parse(ctx2, StringView(txt.data(), txt.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    const String txt2 = ceir::print(ctx2, *pr.module, &alloc);
    CHECK(StringView(txt.data(), txt.size()) == StringView(txt2.data(), txt2.size()));   // fixpoint
    CHECK(ctx2.find_frame_misuse(*pr.module).kind == ceir::FrameMisuseKind::None);        // and re-verifies clean

    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc2)); // the backward recovers the history read name from the wrapped declare
    const String toml_b = emit_frame_toml(desc2, &alloc);
    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size())); // ping-pong survives round-trip
}

// CEIR-15c-1b (NEW-IN-CEIR §4): a hand-authored `rw` operand maps to BOTH a read AND a write ref. The FORWARD converter
// never EMITS rw (a desc read-modify-write rides two operands, w+r), so the only way to exercise the backward converter's
// rw path is to inject the token — forward build_scene, then flip the `forward` pass' scene operand from `r` to `rw`. The
// old last-char-wins token scanner collapsed rw->'w' and silently DROPPED the read half.
TEST_CASE("ceir 15c: a hand-authored rw operand maps to both a read and a write", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_rw");
    build_scene(fb); // the `forward` pass: writes @output, reads scene, draw_list opaque -> operands [@output(w), scene(r), opaque(r)]

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const fwd = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("forward"));
    REQUIRE(fwd != nullptr);
    ctx.set_attr(fwd, "access", ctx.attr_string(StringView("w,rw,r"))); // scene becomes an IN-PLACE read-modify-write
    // scene is a TRANSIENT (not lifetime=history), so the 15c-1a guards 3/4 do not fire on the rw operand — this is clean.
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None);

    FrameGraphDesc desc1(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc1));
    const FramePassDesc* const post = find_pass_desc(desc1, StringView("forward"));
    REQUIRE(post != nullptr);
    CHECK(ref_list_contains(post->writes, StringView("scene"))); // the write half
    CHECK(ref_list_contains(post->reads, StringView("scene")));  // ⭐ the read half the old scanner dropped
    // an in-place RMW of a transient is a VALID graph — the cycle sort skips the same pass (a==b), so read+write of one
    // resource by one pass is no self-cycle.
    CHECK(validate_frame_graph(desc1) == FrameCookError::Ok);

    // canonicalization fixpoint: ceir->desc->ceir is deliberately NOT byte-identical for rw input (the forward splits the
    // rw operand into two, w+r); but desc->ceir->desc IS stable after that one canonicalizing round.
    ceir::Context       ctx2(&alloc);
    ceir::Module* const m2 = to_ceir_frame(desc1, ctx2);
    REQUIRE(m2 != nullptr);
    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx2, *m2, &alloc, desc2));
    const String a = emit_frame_toml(desc1, &alloc);
    const String b = emit_frame_toml(desc2, &alloc);
    CHECK(StringView(a.data(), a.size()) == StringView(b.data(), b.size())); // the canonical two-operand form is stable
}

// CEIR-15c-1d-1: the PROGRAM-CONTRACT layer. validate_ceir_frame materializes the desc (from_ceir_frame) and runs the SHARED
// pass_contract_diag per pass; the specific FrameCookError rides FrameSemanticDiag::contract. The oracle collapses to
// `d.contract == cook_verdict` (the SAME FrameCookError, no mapping table to desync). Family 1: the raster/fullscreen contracts.
TEST_CASE("ceir 15c: validate_ceir_frame catches MissingDrawList (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_dl");
    const crd::u32          geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_writes(geo, "@output"); // ⛔ a scene-raster pass with NO draw list

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::MissingDrawList);
    CHECK(d.op != nullptr); // points at the offending frame.pass op
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::MissingDrawList); // ⭐ the verdict IS the oracle
}

TEST_CASE("ceir 15c: validate_ceir_frame catches MissingShader (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_shader");
    const crd::u32          fs = fb.add_pass("post", "raster.fullscreen");
    fb.pass_writes(fs, "@output"); // ⛔ a fullscreen pass with NO shader

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::MissingShader);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::MissingShader);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches LoadNeedsGeometry (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_load");
    const crd::u32          fs = fb.add_pass("post", "raster.fullscreen");
    fb.pass_shader(fs, "fs_shader");                                        // has a shader (so NOT MissingShader)
    fb.pass_writes(fs, "@output");
    set_pass_flag(fb.desc().passes[fs], StringView(pp::kLoad), true);       // ⛔ `load` is honoured only by a plain geometry pass

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::LoadNeedsGeometry);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::LoadNeedsGeometry);
}

// Family 2 — the RAY-TRACING contracts. raygen/miss/closest_hit are STRING params (the 2c param bag). The AS check is the
// STRONG param test: all three programs must round-trip for the three-check to pass, so a lost program surfaces as
// RtPipelineNeedsThree != the expected RayTraceNeedsAccel (a loud failure), not a silent skip.
TEST_CASE("ceir 15c: validate_ceir_frame catches RtPipelineNeedsThree (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_rt3");
    const crd::u32          rt = fb.add_pass("trace", "raytrace.pipeline");
    set_pass_str(fb.desc().passes[rt], StringView(pp::kRaygen), StringView("rgen")); // ⛔ raygen only — miss + closest_hit absent
    fb.pass_writes(rt, "@output");

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::RtPipelineNeedsThree);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::RtPipelineNeedsThree);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches RayTraceNeedsAccel (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_rtaccel");
    const crd::u32          rt = fb.add_pass("trace", "raytrace.pipeline");
    set_pass_str(fb.desc().passes[rt], StringView(pp::kRaygen), StringView("rgen")); // all three programs present …
    set_pass_str(fb.desc().passes[rt], StringView(pp::kMiss), StringView("rmiss"));
    set_pass_str(fb.desc().passes[rt], StringView(pp::kClosestHit), StringView("rchit"));
    fb.pass_writes(rt, "@output");                                                   // … but reads no acceleration structure

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::RayTraceNeedsAccel);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::RayTraceNeedsAccel);
}

// Family 3 — the INDIRECT-dispatch contracts. `compute.indirect` carries the `indirect` bool flag (2c bag) AND a kernel
// (a symbol attr); both must round-trip or MissingShader fires instead. The pair separates "named no args" from "named a
// non-args resource" — different author mistakes with different fixes.
TEST_CASE("ceir 15c: validate_ceir_frame catches IndirectNeedsArgs (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_indargs");
    const crd::u32          ind = fb.add_pass("cull", "compute.indirect");
    fb.pass_kernel(ind, "cull_cs"); // has a kernel (so NOT MissingShader) …
    fb.pass_writes(ind, "@output"); // … and reads NO resource at all → "named no args"

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::IndirectNeedsArgs);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::IndirectNeedsArgs);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches IndirectArgsNotArgs (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_indwrong");
    fb.add_image("input", gpu::FgImageFormat::RGBA8Unorm, 64U, 64U, true); // an ordinary read, NOT an indirect_args buffer
    const crd::u32 ind = fb.add_pass("cull", "compute.indirect");
    fb.pass_kernel(ind, "cull_cs");
    fb.pass_reads(ind, "input"); // ⛔ reads SOMETHING, but none of it is an indirect_args buffer
    fb.pass_writes(ind, "@output");

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::IndirectArgsNotArgs);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::IndirectArgsNotArgs);
}

// Family 4 — a UTILITY (transfer) contract. A copy/blit/resolve names EXACTLY ONE source; zero reads has nothing to copy.
TEST_CASE("ceir 15c: validate_ceir_frame catches TransferNeedsOneRead (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_copy0");
    const crd::u32          cp = fb.add_pass("blit", "copy");
    fb.pass_writes(cp, "@output"); // ⛔ a copy with a destination but NO source (reads == 0)

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::TransferNeedsOneRead);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::TransferNeedsOneRead);
}

// A clear PRODUCES a target and consumes nothing; a declared read reads as intent and mis-orders the dependency sort.
TEST_CASE("ceir 15c: validate_ceir_frame catches ClearReadsNothing (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_clearread");
    fb.add_image("target", gpu::FgImageFormat::RGBA8Unorm, 64U, 64U, false);
    fb.add_image("src", gpu::FgImageFormat::RGBA8Unorm, 64U, 64U, true);
    const crd::u32 cl = fb.add_pass("clear", "clear");
    fb.pass_writes(cl, "target");
    fb.pass_reads(cl, "src"); // ⛔ a clear consumes nothing

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::ClearReadsNothing);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::ClearReadsNothing);
}

// A present is a SINK: EXACTLY ONE read. Two ⇒ the executor picks one and the author never learns which.
TEST_CASE("ceir 15c: validate_ceir_frame catches PresentNeedsOneRead (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_present2");
    fb.add_image("a", gpu::FgImageFormat::RGBA8Unorm, 64U, 64U, true);
    fb.add_image("b", gpu::FgImageFormat::RGBA8Unorm, 64U, 64U, true);
    const crd::u32 pr = fb.add_pass("present", "present");
    fb.pass_reads(pr, "a");
    fb.pass_reads(pr, "b"); // ⛔ two canvases; a present reads exactly one

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::PresentNeedsOneRead);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::PresentNeedsOneRead);
}

// Family 5 — an AMPLIFICATION contract. A tess/mesh pass needs a draw list OR an explicit `patches`/`groups` count;
// neither dispatches ZERO. Here: a shader (so NOT MissingShader) but no draw list and no count.
TEST_CASE("ceir 15c: validate_ceir_frame catches AmplifyNeedsCount (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_amplify");
    const crd::u32          t = fb.add_pass("tess", "raster.tess");
    fb.pass_shader(t, "tess_prog"); // has a program (so NOT MissingShader) …
    fb.pass_writes(t, "@output");   // … but neither a draw list nor a patches/groups count

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::AmplifyNeedsCount);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::AmplifyNeedsCount);
}

// A VISIBILITY-BUFFER pass MUST write R32Uint — an id in RGBA8 aliases past 255 and shades the WRONG mesh.
TEST_CASE("ceir 15c: validate_ceir_frame catches VisbufferNeedsUintTarget (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_visbuf");
    fb.add_image("vis", gpu::FgImageFormat::RGBA8Unorm, 64U, 64U, false); // ⛔ not R32Uint
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 vb = fb.add_pass("visbuffer", "raster.visbuffer");
    fb.pass_draw_list(vb, "opaque"); // has a draw list (so NOT MissingDrawList) …
    fb.pass_writes(vb, "vis");       // … but the target is not R32Uint
    fb.pass_param(vb, "clear_id", 7.0); // ⛔ CEIR-16z-3b: exercises the CLEAR_ID arm of the union trigger (a scene pass DECLARING
                                        // id semantics on a non-uint target); REN-38-A11 (gpu-context) covers the PROCEDURAL arm.

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::VisbufferNeedsUintTarget);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::VisbufferNeedsUintTarget);
}

// Only COMPUTE-shaped work runs on the async-compute queue. `queue` is a FramePassDesc field the converter carries (an
// attr on the frame.pass op); a raster pass that asked for async cannot rasterise there.
TEST_CASE("ceir 15c: validate_ceir_frame catches AsyncQueueNeedsCompute (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_async");
    const crd::u32          dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");                   // dodges MissingDrawList
    fb.pass_writes(geo, "@output");
    fb.desc().passes[geo].queue = FrameQueue::Async;     // ⛔ a raster pass on the async-compute queue

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::AsyncQueueNeedsCompute);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::AsyncQueueNeedsCompute);
}

// A COMPOSITE with no blend OVERWRITES — the "transparency layer is opaque" bug this kind exists to prevent. Also
// self-verifies the `composite` flag round-trips: if it were lost, this reduces to a valid fullscreen pass (no diag).
TEST_CASE("ceir 15c: validate_ceir_frame catches CompositeNeedsBlend (program-contract, oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pc_composite");
    const crd::u32          c = fb.add_pass("composite", "raster.composite");
    fb.pass_shader(c, "comp_prog"); // has a program (so NOT MissingShader) …
    fb.pass_writes(c, "@output");   // … but declares no blend

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::CompositeNeedsBlend);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::CompositeNeedsBlend);
}

// ── CEIR-15c-1d-6: the closed-vocab RANGE-CHECKS. The ceir.frame carries enum params as raw ints; the desc-side parse
// rejects bad enum STRINGS and emit NORMALIZES a bad int to a default, so there is NO oracle (no cook_verdict compare) — the
// third leg is the op-pointer + the specific FrameCookError on `.contract`. Injection is DESC-LEVEL: set_pass_enum authors a
// (non-default) garbage value that to_ceir_frame carries FAITHFULLY (with its `pt:` type), so from_ceir_frame reconstructs it
// and enum_vocab_diag catches it — simpler than overwriting the ceir attr, and the pt: is never missing. ──
namespace
{
// A structurally-clean scene-raster pass (draw list + writes @output) so a vocab test can attach ONE garbage enum param and
// reach validate_ceir_frame's program-contract loop. The vocab check is executor-agnostic — it flags the param VALUE
// regardless of which pass carries it, so the simplest clean pass suffices.
crd::u32 clean_geo_with_dl(FrameGraphBuilder& fb)
{
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 g = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(g, "opaque");
    fb.pass_writes(g, "@output");
    return g;
}
// Author `name = 42` (out of range for EVERY one of these closed vocabs, which all have < 42 values) on a clean pass and
// assert validate_ceir_frame reports UnknownEnumParam carrying `expected`.
void check_bad_vocab(crd::containers::StringView param, FrameCookError expected)
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_vocab");
    const crd::u32          g = clean_geo_with_dl(fb);
    set_pass_enum(fb.desc().passes[g], param, 42U); // ⛔ not a valid value for this vocab
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownEnumParam);
    CHECK(d.contract == expected);
    CHECK(d.op != nullptr); // points at the pass carrying the bad param
}
// The ACCEPT direction: author the HIGHEST VALID value for a vocab and assert the frame is clean. This is what a cross-mapped
// table row or an off-by-one validator REJECTS — the 42-reject tests can't catch it (42 is invalid under every validator, and
// the error label comes from the row, not the validator, so a mis-wired row still "passes"). Non-zero maxes ⇒ pass_has goes
// true and the accept path actually executes.
void check_good_vocab(crd::containers::StringView param, crd::u32 max_valid)
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_vocab_ok");
    const crd::u32          g = clean_geo_with_dl(fb);
    set_pass_enum(fb.desc().passes[g], param, max_valid); // the highest in-range value
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None); // NOT false-rejected
}
// 6b: inject a garbage STRING on a converted op's `attr` (the KindLifetimeMismatch pattern — the forward only emits valid
// strings and from_ceir_frame NORMALIZES bad ones, so the op attr is the only place the vocab can be tested).
void check_bad_str_vocab(crd::containers::StringView op_kind, crd::containers::StringView op_name, const char* attr,
                         FrameCookError expected)
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_str");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const o = find_graph_op(ctx, *m, op_kind, op_name);
    REQUIRE(o != nullptr);
    ctx.set_attr(o, attr, ctx.attr_string(StringView("frobnicate"))); // ⛔ not in the vocab
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownEnumParam);
    CHECK(d.contract == expected);
    CHECK(d.op == o);
}
// 6b accept: a VALID non-default string must not be false-rejected. The round-trip trick's failure mode is an X_str/X_from
// spelling asymmetry on a SPECIFIC value (build_scene only exercises front_to_back/frustum), so cover the others.
void check_good_str_vocab(crd::containers::StringView op_kind, crd::containers::StringView op_name, const char* attr,
                          const char* good)
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_str_ok");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const o = find_graph_op(ctx, *m, op_kind, op_name);
    REQUIRE(o != nullptr);
    ctx.set_attr(o, attr, ctx.attr_string(StringView(good)));
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None); // a legit value is NOT rejected
}
} // namespace

TEST_CASE("ceir 15c: validate_ceir_frame closed-vocab range-checks (int-carried pass params)", "[framecook][ceir][frame]")
{
    SECTION("blend") { check_bad_vocab(StringView(pp::kBlendSlot[0]), FrameCookError::UnknownBlend); }
    SECTION("blend3") { check_bad_vocab(StringView(pp::kBlendSlot[3]), FrameCookError::UnknownBlend); }
    SECTION("depth_compare") { check_bad_vocab(StringView(pp::kDepthCompare), FrameCookError::UnknownCompare); }
    SECTION("stencil_compare") { check_bad_vocab(StringView(pp::kStencilCompare), FrameCookError::UnknownCompare); }
    SECTION("material_pass") { check_bad_vocab(StringView(pp::kMaterialPass), FrameCookError::UnknownMaterialPass); }
    SECTION("sampler_min_filter") { check_bad_vocab(StringView(pp::kSamplerMin), FrameCookError::UnknownSamplerFilter); }
    SECTION("sampler_mag_filter") { check_bad_vocab(StringView(pp::kSamplerMag), FrameCookError::UnknownSamplerFilter); }
    SECTION("sampler_mip_filter") { check_bad_vocab(StringView(pp::kSamplerMip), FrameCookError::UnknownSamplerFilter); }
    SECTION("filter") { check_bad_vocab(StringView(pp::kFilter), FrameCookError::UnknownFilter); }
    SECTION("sampler_address") { check_bad_vocab(StringView(pp::kSamplerAddr), FrameCookError::UnknownSamplerAddress); }
    SECTION("face_cull") { check_bad_vocab(StringView(pp::kFaceCull), FrameCookError::UnknownFaceCull); }
    SECTION("front_face") { check_bad_vocab(StringView(pp::kFrontFace), FrameCookError::UnknownFrontFace); }
    SECTION("stencil_fail") { check_bad_vocab(StringView(pp::kStencilFail), FrameCookError::UnknownStencilOp); }
    SECTION("stencil_depth_fail") { check_bad_vocab(StringView(pp::kStencilDepthFail), FrameCookError::UnknownStencilOp); }
    SECTION("stencil_pass") { check_bad_vocab(StringView(pp::kStencilPass), FrameCookError::UnknownStencilOp); }
    SECTION("shading_rate") { check_bad_vocab(StringView(pp::kShadingRate), FrameCookError::UnknownShadingRate); }
    SECTION("rate_combiner") { check_bad_vocab(StringView(pp::kRateCombiner), FrameCookError::UnknownRateCombiner); }
    SECTION("conservative") { check_bad_vocab(StringView(pp::kConservative), FrameCookError::UnknownConservative); }
}

// ⛔ the u8-WRAP hole: these enums are u8, so static_cast<BlendMode>(256) wraps to 0 = Opaque = VALID. The `v > 0xFF` guard
// rejects it before the cast — this test would PASS falsely (no diag) without that guard.
TEST_CASE("ceir 15c: closed-vocab range-check rejects a u8-wrapping enum int (256)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_vocab_wrap");
    const crd::u32          g = clean_geo_with_dl(fb);
    set_pass_enum(fb.desc().passes[g], StringView(pp::kBlendSlot[0]), 256U); // ⛔ wraps to 0=Opaque without the guard
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownEnumParam);
    CHECK(d.contract == FrameCookError::UnknownBlend);
}

// The MOTIVATING hole: a garbage blend on a COMPOSITE pass. `pass_contract_diag` reads blend0 as static_cast<BlendMode>(42) =
// a non-Opaque value, so garbage would SATISFY CompositeNeedsBlend ("it has a blend"). The vocab check running FIRST closes
// it — the frame is rejected as UnknownEnumParam, never mis-accepted as a valid composite.
TEST_CASE("ceir 15c: a garbage blend does NOT satisfy CompositeNeedsBlend (vocab runs before the contract)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_vocab_composite");
    const crd::u32          c = fb.add_pass("composite", "raster.composite");
    fb.pass_shader(c, "comp_prog");
    fb.pass_writes(c, "@output");
    set_pass_u32(fb.desc().passes[c], StringView(pp::kBlendCount), 1U);
    set_pass_enum(fb.desc().passes[c], StringView(pp::kBlendSlot[0]), 42U); // ⛔ garbage that reads as "a non-Opaque blend"

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownEnumParam); // NOT None, NOT CompositeNeedsBlend-clean
    CHECK(d.contract == FrameCookError::UnknownBlend);
}

TEST_CASE("ceir 15c: closed-vocab range-checks ACCEPT the max valid value (no false reject / cross-map)", "[framecook][ceir][frame]")
{
    SECTION("blend") { check_good_vocab(StringView(pp::kBlendSlot[0]), 6U); }              // RevealComposite
    SECTION("blend3") { check_good_vocab(StringView(pp::kBlendSlot[3]), 6U); }
    SECTION("depth_compare") { check_good_vocab(StringView(pp::kDepthCompare), 7U); }        // Always
    SECTION("stencil_compare") { check_good_vocab(StringView(pp::kStencilCompare), 7U); }
    SECTION("material_pass") { check_good_vocab(StringView(pp::kMaterialPass), 4U); }        // Forward
    SECTION("sampler_min_filter") { check_good_vocab(StringView(pp::kSamplerMin), 1U); }     // Linear
    SECTION("sampler_mag_filter") { check_good_vocab(StringView(pp::kSamplerMag), 1U); }
    SECTION("sampler_mip_filter") { check_good_vocab(StringView(pp::kSamplerMip), 1U); }
    SECTION("filter") { check_good_vocab(StringView(pp::kFilter), 1U); }
    SECTION("sampler_address") { check_good_vocab(StringView(pp::kSamplerAddr), 3U); }       // Mirror
    SECTION("face_cull") { check_good_vocab(StringView(pp::kFaceCull), 2U); }                // Front
    SECTION("front_face") { check_good_vocab(StringView(pp::kFrontFace), 1U); }              // Clockwise
    SECTION("stencil_fail") { check_good_vocab(StringView(pp::kStencilFail), 7U); }          // DecrWrap
    SECTION("stencil_depth_fail") { check_good_vocab(StringView(pp::kStencilDepthFail), 7U); }
    SECTION("stencil_pass") { check_good_vocab(StringView(pp::kStencilPass), 7U); }
    SECTION("shading_rate") { check_good_vocab(StringView(pp::kShadingRate), 6U); }          // Rate4x4
    SECTION("rate_combiner") { check_good_vocab(StringView(pp::kRateCombiner), 4U); }        // Mul
    SECTION("conservative") { check_good_vocab(StringView(pp::kConservative), 2U); }         // Underestimate
}

// ── CEIR-15c-1d-6b: the STRING-carried (sort/cull on draw_list, for_each/queue on pass) + `format` (int on the resource)
// closed vocabs. Injection is OP-ATTR-LEVEL: the forward converter only EMITS valid strings/ints, and from_ceir_frame's
// backward readers NORMALIZE a bad string to a default, so a desc-based check can't see the garbage. NO oracle. ──
TEST_CASE("ceir 15c: 6b format closed-vocab (int on the resource op)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_format");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const sc = find_graph_op(ctx, *m, StringView("resource.declare"), StringView("scene"));
    REQUIRE(sc != nullptr);
    ctx.set_attr(sc, "format", ctx.attr_int(99)); // ⛔ only 17 FgImageFormats exist
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownEnumParam);
    CHECK(d.contract == FrameCookError::UnknownFormat);
    CHECK(d.op == sc);
}

TEST_CASE("ceir 15c: 6b string-carried closed vocabs (sort/cull on draw_list, for_each/queue on pass)", "[framecook][ceir][frame]")
{
    SECTION("sort") { check_bad_str_vocab(StringView("frame.draw_list"), StringView("opaque"), "sort", FrameCookError::UnknownSort); }
    SECTION("cull") { check_bad_str_vocab(StringView("frame.draw_list"), StringView("opaque"), "cull", FrameCookError::UnknownCull); }
    SECTION("for_each") { check_bad_str_vocab(StringView("frame.pass"), StringView("geometry"), "for_each", FrameCookError::UnknownForEach); }
    SECTION("queue") { check_bad_str_vocab(StringView("frame.pass"), StringView("geometry"), "queue", FrameCookError::UnknownQueue); }
}

// ACCEPT: sort/cull/format accept ride the build_scene positive canary (valid non-default values). for_each/queue need their
// own — a valid non-empty for_each string round-trips clean, and queue=async on a COMPUTE pass is valid (AsyncQueueNeedsCompute
// is a SEPARATE semantic layer, so this confirms the vocab layer doesn't over-reject a legitimate async request).
TEST_CASE("ceir 15c: 6b accepts a valid non-empty for_each string", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_fe_ok");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const g = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("geometry"));
    REQUIRE(g != nullptr);
    ctx.set_attr(g, "for_each", ctx.attr_string(StringView("cube.faces"))); // a VALID generator
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None);
}

TEST_CASE("ceir 15c: 6b accepts queue=async on a compute pass (vocab layer does not over-reject)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_queue_ok");
    const crd::u32          cs = fb.add_pass("cs", "compute");
    fb.pass_kernel(cs, "k");
    fb.pass_writes(cs, "@output");
    fb.desc().passes[cs].queue = FrameQueue::Async; // valid: a compute pass CAN run async
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None);
}

// ACCEPT boundary for the hand-written format switch: the build_scene canary only exercises format=0 (RGBA8Unorm), so a
// missing last case would slip through. Author the MAX valid format (D32FloatS8 = 16) and assert it is NOT false-rejected.
TEST_CASE("ceir 15c: 6b accepts the max valid format (D32FloatS8), catching a missing switch case", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_format_ok");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const sc = find_graph_op(ctx, *m, StringView("resource.declare"), StringView("scene"));
    REQUIRE(sc != nullptr);
    ctx.set_attr(sc, "format", ctx.attr_int(static_cast<crd::i64>(gpu::FgImageFormat::D32FloatS8))); // the highest valid value
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None);
}

// The format check is wired into BOTH resource branches; the declare test above only exercises the declare branch. An
// unreferenced ExternalTexture import (structurally fine — imports are exempt from producer rules) drives the IMPORT branch.
TEST_CASE("ceir 15c: 6b format check fires on the resource.import branch too", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_6b_import_fmt");
    build_scene(fb);
    FrameResourceDesc ext(&alloc);
    ext.name.append("ui_atlas", 8);
    ext.kind    = FrameResourceKind::ExternalTexture; // an import (external kind), unreferenced by any pass
    ext.format  = gpu::FgImageFormat::RGBA8Unorm;
    ext.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(ext));
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const im = find_graph_op(ctx, *m, StringView("resource.import"), StringView("ui_atlas"));
    REQUIRE(im != nullptr);
    ctx.set_attr(im, "format", ctx.attr_int(99)); // ⛔ scene's valid format passes first; the import fires
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownEnumParam);
    CHECK(d.contract == FrameCookError::UnknownFormat);
    CHECK(d.op == im); // the IMPORT op, not the declare
}

TEST_CASE("ceir 15c: 6b accepts the non-default valid string vocabs (round-trip symmetry)", "[framecook][ceir][frame]")
{
    SECTION("cull frustum_occlusion") { check_good_str_vocab(StringView("frame.draw_list"), StringView("opaque"), "cull", "frustum_occlusion"); }
    SECTION("sort material") { check_good_str_vocab(StringView("frame.draw_list"), StringView("opaque"), "sort", "material"); }
}

// ── CEIR-15c converter losses 2b + 2d (round-trip-identity gate: emit_frame_toml(desc) == emit_frame_toml(to→from(desc))). ──
// 2b: the per-operand `[$index]` subscript (FrameResourceRef::indexed) — carried as a parallel per-operand char string on the
// frame.pass op, aligned with `access`.
TEST_CASE("ceir 15c 2b: the [$index] subscript round-trips per operand", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_2b_indexed");
    build_scene(fb);
    fb.desc().passes[0].writes[0].indexed = true; // geometry writes scene[$index]
    fb.desc().passes[1].reads[0].indexed  = true; // forward reads scene[$index]

    const String        toml_a = emit_frame_toml(fb.desc(), &alloc);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None);
    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc2));
    const String toml_b = emit_frame_toml(desc2, &alloc);
    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size())); // ⭐ scene[$index] survives
    REQUIRE(desc2.passes.size() == 2U);
    CHECK(desc2.passes[0].writes[0].indexed);  // the marked write is indexed
    CHECK(desc2.passes[1].reads[0].indexed);   // the marked read is indexed
    CHECK(!desc2.passes[1].writes[0].indexed); // …and the @output write stays UNindexed (the parallel alignment doesn't leak)
}

// 2d: the graph-level fields — requires_caps (a comma-joined string), fallback, memory_budget_bytes — as attrs on frame.graph.
TEST_CASE("ceir 15c 2d: the graph-level caps/fallback/budget round-trip", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_2d_caps");
    build_scene(fb);
    fb.requires_capability("mesh_shader");
    fb.requires_capability("ray_tracing");
    fb.fallback_to("simple_forward");
    fb.desc().memory_budget_bytes = 64ULL * 1024ULL * 1024ULL; // a whole MB (emit_frame_toml serializes at MB granularity)

    const String        toml_a = emit_frame_toml(fb.desc(), &alloc);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc2));
    const String toml_b = emit_frame_toml(desc2, &alloc);
    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size())); // ⭐ all three survive
    REQUIRE(desc2.requires_caps.size() == 2U);
    CHECK(StringView(desc2.requires_caps[0].data(), desc2.requires_caps[0].size()) == StringView("mesh_shader"));
    CHECK(StringView(desc2.requires_caps[1].data(), desc2.requires_caps[1].size()) == StringView("ray_tracing"));
    CHECK(StringView(desc2.fallback.data(), desc2.fallback.size()) == StringView("simple_forward"));
    CHECK(desc2.memory_budget_bytes == 64ULL * 1024ULL * 1024ULL);
}

// 2b made two pass-contract rows REACHABLE through validate_ceir_frame (before, from_ceir_frame always produced indexed=false
// so they were dead). Both are ORACLE rows — emit writes `[$index]`, parse_ref reads it, so cook_verdict agrees.
TEST_CASE("ceir 15c 2b: an indexed ref with no for_each is IndexWithoutForEach (now-live, oracle)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_idx_nofe");
    FrameResourceDesc       atlas(&alloc);
    atlas.name.append("atlas", 5);
    atlas.format  = gpu::FgImageFormat::RGBA8Unorm;
    atlas.width   = 256U;
    atlas.height  = 256U;
    atlas.layers  = 4U; // LAYERED, so SubscriptOnNonLayered can't fire first
    atlas.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(atlas));
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_reads(geo, "atlas", /*indexed=*/true); // ⛔ atlas[$index] with NO for_each
    fb.pass_writes(geo, "@output");

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::IndexWithoutForEach);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::IndexWithoutForEach); // 2b carries indexed → the oracle agrees
}

TEST_CASE("ceir 15c 2b: an indexed ref on a layers==1 image is SubscriptOnNonLayered (now-live, oracle)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_idx_flat");
    fb.add_image("tex", gpu::FgImageFormat::RGBA8Unorm, 256U, 256U, /*sampled=*/true); // layers defaults to 1
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 geo = fb.add_pass("geometry", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_reads(geo, "tex", /*indexed=*/true); // ⛔ tex[$index] but tex has layers==1
    fb.pass_writes(geo, "@output");
    fb.pass_for_each(geo, FrameForEach::CubeFaces); // for_each SET so IndexWithoutForEach can't fire first

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ProgramContract);
    CHECK(d.contract == FrameCookError::SubscriptOnNonLayered);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::SubscriptOnNonLayered);
}

// 2e (the PERMANENT contract): REN-37.6 composition (includes/anchors/injects) is a §39 named-forward SUBGRAPH capability
// that flatten_frame_graph resolves into plain passes BEFORE conversion (pipeline: parse -> flatten -> convert). to_ceir_frame
// REJECTS a desc that still carries it — a caller that skipped the flatten. NOT an interim loss: carrying composition into
// ceir.frame would force composition special-cases into every 15d analysis, which is exactly what REN-37.6 exists to prevent.
TEST_CASE("ceir 15c 2e: to_ceir_frame rejects a desc carrying composition (flatten-before-convert is the contract)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_2e_reject");
    build_scene(fb);
    FrameIncludeDesc inc(&alloc);
    inc.graph.append("shared_shadows", 14);
    inc.as.append("sh", 2);
    fb.desc().includes.push_back(static_cast<FrameIncludeDesc&&>(inc));
    ceir::Context ctx(&alloc);
    CHECK(to_ceir_frame(fb.desc(), ctx) == nullptr); // ⛔ composition not yet representable — rejected, not dropped
}

// ── CEIR-15d-1: frame.pass barriers are DERIVED PER-OPERAND by collect_block_hazards (the §159 realization — the frame
// graph's hand-rolled barrier pass IS a CEIR analysis pass). op_access_at narrows frame.pass's ambient [GPUCommand,
// MemoryReadWrite] to one Memory access per operand tokened by `access` (GPUCommand suppressed; draw-list operands inert). ──
// build_scene: geometry WRITES scene, forward READS it → exactly one RAW (was WAW under the pre-15d-1 ambient baseline).
TEST_CASE("ceir 15d-1: build_scene derives one precise RAW (geometry->forward on scene)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d_scene");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const geo = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("geometry"));
    ceir::Operation* const fwd = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("forward"));
    REQUIRE(geo != nullptr);
    REQUIRE(fwd != nullptr);
    ceir::Block* const gb = graph_block(ctx, *m);
    REQUIRE(gb != nullptr);
    crd::containers::Array<ceir::Hazard> hz(&alloc);
    ctx.collect_block_hazards(*gb, hz);
    REQUIRE(hz.size() == 1U);                       // ONLY geometry↔forward share a memory resource (the draw list is inert)
    CHECK(hz[0].kind == ceir::HazardKind::Raw);     // ⭐ precise: WRITE(scene) then READ(scene) — NOT the ambient WAW
    CHECK(hz[0].before == geo);                     // geometry (the writer) precedes
    CHECK(hz[0].after == fwd);                      // forward (the reader)
}

// The GPUCommand-suppression + draw-list-skip proof (now isolatable, per the design note): two passes that share ONLY the
// draw list (each writes a DIFFERENT transient) → ZERO hazards. Under the ambient baseline this was WAW (GPUCommand's whole-
// class Gpu write + whole-class Memory), so a non-zero result here means GPUCommand leaked or a draw-list operand hazarded.
TEST_CASE("ceir 15d-1: memory-disjoint passes sharing only a draw list derive ZERO hazards", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d_disjoint");
    fb.add_scaled_image("a", gpu::FgImageFormat::RGBA8Unorm, 1.0F, true);
    fb.add_scaled_image("b", gpu::FgImageFormat::RGBA8Unorm, 1.0F, true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 pa = fb.add_pass("passa", "raster.geometry");
    fb.pass_draw_list(pa, "opaque");
    fb.pass_writes(pa, "a");
    const crd::u32 pb = fb.add_pass("passb", "raster.geometry");
    fb.pass_draw_list(pb, "opaque");
    fb.pass_writes(pb, "b");

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Block* const gb = graph_block(ctx, *m);
    REQUIRE(gb != nullptr);
    crd::containers::Array<ceir::Hazard> hz(&alloc);
    ctx.collect_block_hazards(*gb, hz);
    CHECK(hz.size() == 0U); // ⭐ no shared memory ⇒ no barrier (GPUCommand suppressed, draw-list operand inert)
}

// build_taa (the real TAA graph, a history resource in play): the narrowing stays precise — geometry WRITES scene, taa READS
// it → the scene RAW; taa's own read-of-prev-history vs write-of-curr-history does NOT manufacture extra hazards.
TEST_CASE("ceir 15d-1: build_taa derives the scene RAW with a history resource present", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d_taa");
    build_taa(fb, &alloc);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const geo = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("geometry"));
    ceir::Operation* const taa = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("taa"));
    REQUIRE(geo != nullptr);
    REQUIRE(taa != nullptr);
    ceir::Block* const gb = graph_block(ctx, *m);
    REQUIRE(gb != nullptr);
    crd::containers::Array<ceir::Hazard> hz(&alloc);
    ctx.collect_block_hazards(*gb, hz);
    REQUIRE(hz.size() == 1U);                    // geometry↔taa on scene; history read/write are intra-taa, not a pair
    CHECK(hz[0].kind == ceir::HazardKind::Raw);
    CHECK(hz[0].before == geo);
    CHECK(hz[0].after == taa);
}

// ⭐ Fork B at the HAZARD level, ISOLATED inter-op: a WRITER pass writes `history` (this frame) and a separate READER pass
// reads it (prev frame, routed through frame.history since history is PingPong). The reader's root (frame.history result) !=
// the writer's root (history declare) → NO hazard. Under the ambient baseline these all-pairs-WAW'd; a naive same-Value
// wiring would derive a FALSE intra-frame RAW/WAR. The frame.history op fixes it STRUCTURALLY — proven here for the first time.
TEST_CASE("ceir 15d-1: history read(prev) vs write(curr) on separate passes derives NO hazard (Fork B)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d_forkb");
    FrameResourceDesc       h(&alloc);
    h.name.append("history", 7);
    h.kind    = FrameResourceKind::PingPongImage;
    h.format  = gpu::FgImageFormat::RGBA8Unorm;
    h.width   = 1920U;
    h.height  = 1080U;
    h.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(h));
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 pw = fb.add_pass("writer", "raster.geometry");
    fb.pass_draw_list(pw, "opaque");
    fb.pass_writes(pw, "history"); // writes THIS frame's history
    fb.pass_writes(pw, "@output");
    const crd::u32 pr = fb.add_pass("reader", "raster.geometry");
    fb.pass_draw_list(pr, "opaque");
    fb.pass_reads(pr, "history"); // reads the PREVIOUS frame (converter routes through frame.history — PingPong)

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Block* const gb = graph_block(ctx, *m);
    REQUIRE(gb != nullptr);
    crd::containers::Array<ceir::Hazard> hz(&alloc);
    ctx.collect_block_hazards(*gb, hz);
    CHECK(hz.size() == 0U); // ⭐ read-of-prev vs write-of-curr are DIFFERENT roots — no false intra-frame hazard
}

// ── CEIR-15d-3: the LIFETIME/aliasing analysis (compute_block_lifetimes, CEIR-12c) also runs on the frame.graph block —
// for FREE via 15d-1: op_has_ambient_mem_or_universe routes through op_access_at, so a narrowed frame.pass is NOT flagged
// ambient (its accesses have a concrete resource, not nullptr/whole-class), and each transient's `last` is its ACTUAL last
// use, not extended to every pass. ──
TEST_CASE("ceir 15d-3: compute_block_lifetimes derives a precise range for a frame transient", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d3_scene");
    build_scene(fb);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Block* const gb = graph_block(ctx, *m);
    REQUIRE(gb != nullptr);
    crd::containers::Array<ceir::ResourceLifetime> lt(&alloc);
    ctx.compute_block_lifetimes(*gb, lt);
    REQUIRE(lt.size() == 1U); // `scene` is the ONE transient declare; @output is an IMPORT (the planner never plans imports)
    CHECK(lt[0].lifetime == ceir::ResourceLifetimeClass::Transient);
    CHECK(lt[0].first < lt[0].last); // a real live range: declared, then WRITTEN by geometry, READ by forward
}

// The narrowing makes lifetimes PRECISE, not ambient-tied: two memory-disjoint transients keep DISTINCT last-uses (a used at
// passa, b at passb). Under the ambient baseline, every pass would extend EVERY range to its own position → both `last` tie
// at the final pass. So `a.last < b.last` is the precise-lifetime proof (the aliasing analogue of the disjoint-hazard test).
TEST_CASE("ceir 15d-3: narrowing keeps disjoint transient lifetimes distinct (not ambient-tied)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d3_disjoint");
    fb.add_scaled_image("a", gpu::FgImageFormat::RGBA8Unorm, 1.0F, true);
    fb.add_scaled_image("b", gpu::FgImageFormat::RGBA8Unorm, 1.0F, true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 pa = fb.add_pass("passa", "raster.geometry");
    fb.pass_draw_list(pa, "opaque");
    fb.pass_writes(pa, "a");
    const crd::u32 pb = fb.add_pass("passb", "raster.geometry");
    fb.pass_draw_list(pb, "opaque");
    fb.pass_writes(pb, "b");

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const da = find_graph_op(ctx, *m, StringView("resource.declare"), StringView("a"));
    ceir::Operation* const db = find_graph_op(ctx, *m, StringView("resource.declare"), StringView("b"));
    REQUIRE(da != nullptr);
    REQUIRE(db != nullptr);
    ceir::Block* const gb = graph_block(ctx, *m);
    REQUIRE(gb != nullptr);
    crd::containers::Array<ceir::ResourceLifetime> lt(&alloc);
    ctx.compute_block_lifetimes(*gb, lt);
    REQUIRE(lt.size() == 2U); // a and b (both transient declares)
    const ceir::ResourceLifetime* la = nullptr;
    const ceir::ResourceLifetime* lb = nullptr;
    for (crd::usize i = 0; i < lt.size(); ++i)
    {
        if (lt[i].declare == da) { la = &lt[i]; }
        if (lt[i].declare == db) { lb = &lt[i]; }
    }
    REQUIRE(la != nullptr);
    REQUIRE(lb != nullptr);
    CHECK(la->last < lb->last); // ⭐ precise: a's last use (passa) precedes b's (passb); ambient would TIE them at the last pass
}

// ── CEIR-15d-2b: the graph-level DependencyCycle — validate_ceir_frame shares the ONE dependency_cycle_diag with the desc-side
// cook pipeline (extract-and-share), so it is an ORACLE row (d.contract == cook_verdict). A real single-frame cycle: passA
// READS y (produced only by the LATER passB — an unsatisfiable forward reference) + WRITES x; passB READS x (from passA) +
// WRITES y → edges A→B (x) and B→A (y), a Kahn topo-sort failure. ──
TEST_CASE("ceir 15d-2: validate_ceir_frame catches a DependencyCycle (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d2_cycle");
    fb.add_scaled_image("x", gpu::FgImageFormat::RGBA8Unorm, 1.0F, true);
    fb.add_scaled_image("y", gpu::FgImageFormat::RGBA8Unorm, 1.0F, true);
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 pa = fb.add_pass("passa", "raster.geometry"); // authored FIRST
    fb.pass_draw_list(pa, "opaque");
    fb.pass_reads(pa, "y");       // ⛔ reads y — produced ONLY by the later passb (a single-frame forward reference)
    fb.pass_writes(pa, "x");
    fb.pass_writes(pa, "@output"); // so no NoOutputPass masks the cycle
    const crd::u32 pb = fb.add_pass("passb", "raster.geometry"); // authored SECOND
    fb.pass_draw_list(pb, "opaque");
    fb.pass_reads(pb, "x");        // reads x (from passa) → A→B; combined with passa's read of y → B→A → CYCLE
    fb.pass_writes(pb, "y");

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // structurally valid — a cycle is a SEMANTIC error
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::DependencyCycle);
    CHECK(d.contract == FrameCookError::DependencyCycle);
    CHECK(d.op == nullptr); // graph-level — no single offending pass
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::DependencyCycle); // ⭐ the oracle agrees
}

// The Fork B control: a TAA history graph reads the PREVIOUS frame's `history` (routed through frame.history) and writes THIS
// frame's — a naive dep graph would see a cycle, but the prev-frame read is a SATISFIABLE frame-start value (PingPong), so NO
// cycle. This is why the ping-pong case never false-positives.
TEST_CASE("ceir 15d-2: a TAA history graph does NOT false-positive DependencyCycle", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_15d2_taa");
    build_taa(fb, &alloc);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind != FrameSemanticKind::DependencyCycle); // the prev-frame history read is a satisfiable frame-start value
    CHECK(d.kind == FrameSemanticKind::None);            // and the TAA graph is otherwise fully valid
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) != FrameCookError::DependencyCycle);
}

// CEIR-15c-1c-1: validate_ceir_frame — the graph-semantic verifier. A well-formed frame (build_scene, and the TAA graph)
// is find_frame_misuse-clean AND validate_ceir_frame-clean (the two layers agree the frame is well-formed).
TEST_CASE("ceir 15c: validate_ceir_frame accepts a well-formed frame", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    {
        FrameGraphBuilder fb(&alloc, "test_frame");
        build_scene(fb);
        ceir::Context       ctx(&alloc);
        ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
        REQUIRE(m != nullptr);
        CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None);
    }
    {
        FrameGraphBuilder fb(&alloc, "test_taa");
        build_taa(fb, &alloc);
        ceir::Context       ctx(&alloc);
        ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
        REQUIRE(m != nullptr);
        CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::None);
    }
}

// A frame whose only pass writes a transient (never @output). find_frame_misuse is CLEAN (structure is fine) — this is a
// SEMANTIC defect validate_ceir_frame owns; the desc-side cook pipeline agrees (NoOutputPass).
TEST_CASE("ceir 15c: validate_ceir_frame catches NoOutputPass (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_no_output");
    build_no_output(fb);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // structurally clean
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::NoOutputPass);

    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::NoOutputPass); // the desc-side oracle agrees
}

// Two passes sharing a name. find_frame_misuse is CLEAN (name uniqueness is not a per-op structural rule) — validate_ceir_frame
// owns the cross-op sweep, pointing at the SECOND collider; the desc-side cook pipeline agrees (parse-time DuplicateName).
TEST_CASE("ceir 15c: validate_ceir_frame catches DuplicateName (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_dup");
    build_scene(fb); // passes: "geometry" and "forward"

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const fwd = find_graph_op(ctx, *m, StringView("frame.pass"), StringView("forward"));
    REQUIRE(fwd != nullptr);
    ctx.set_attr(fwd, "name", ctx.attr_symbol(StringView("geometry"))); // collide with the geometry pass' name
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None);
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::DuplicateName);
    CHECK(d.op == fwd); // points at the SECOND op with the colliding name

    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::DuplicateName); // the desc-side oracle agrees
}

// CEIR-15c-1c-2a: the REN-38-B2 SHAPE (dimension + 3-D depth) must survive the converter round-trip. ⛔ Before this slice
// to_ceir_frame hardcoded Dim2D and dropped both; the round-trip-identity gate false-greened because NO fixture used a
// non-2D resource (the fixture-coverage scar). A 3-D froxel volume re-locks it. Needed by the dimension shape verifiers.
TEST_CASE("ceir 15c: the REN-38-B2 shape (dimension + 3d depth) survives the round-trip", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_volume");
    // a 3-D froxel volume — pushed directly (the builder makes only 2-D images). kind_2d=Tex3D + depth are the carried shape.
    FrameResourceDesc vol(&alloc);
    vol.name.append("froxels", 7);
    vol.format  = gpu::FgImageFormat::RGBA8Unorm;
    vol.width   = 160U;
    vol.height  = 90U;
    vol.kind_2d = gpu::FgImageKind::Tex3D;
    vol.depth   = 64U;
    vol.storage = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(vol));
    const String toml_a = emit_frame_toml(fb.desc(), &alloc);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None);
    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc2));
    const String toml_b = emit_frame_toml(desc2, &alloc);
    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size())); // dimension+depth survive
    REQUIRE(desc2.resources.size() == 1U); // and the reconstructed shape is EXACTLY Tex3D/64 (a direct check, not only toml)
    CHECK(desc2.resources[0].kind_2d == gpu::FgImageKind::Tex3D);
    CHECK(desc2.resources[0].depth == 64U);
}

// CEIR-15c-1c-2 def-use: a ping-pong written but never read (a frame.history-mediated read is what counts) can never
// rotate. validate_ceir_frame walks the pass operands, chasing frame.history back to the declare; the desc-side oracle agrees.
TEST_CASE("ceir 15c: validate_ceir_frame catches PingPongNeedsBothWays (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_pp_writeonly");
    build_pingpong_writeonly(fb, &alloc);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // structurally clean (no direct history read/write)
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::PingPongNeedsBothWays);

    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::PingPongNeedsBothWays);
}

// CEIR-15c-1c-2 def-use: a graph-owned TRANSIENT no pass produces. build_scene (which writes @output, so not NoOutputPass)
// plus an unreferenced transient `orphan` — the only violation is that nothing writes it. persistent/history/import are exempt.
TEST_CASE("ceir 15c: validate_ceir_frame catches ResourceNeverWritten (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_orphan");
    build_scene(fb);
    fb.add_scaled_image("orphan", gpu::FgImageFormat::RGBA8Unorm, 0.5F, /*sampled=*/true); // a transient no pass writes

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // structurally clean (an unreferenced declare is fine)
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::ResourceNeverWritten);

    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::ResourceNeverWritten);
}

// CEIR-15c-1c-2 shape (REN-38-B2): the TransientImage SHAPE verifiers, now unblocked by the 2a dimension/depth carry. Each
// uses a lone mis-shaped resource — the shape block fires before NoOutputPass on BOTH sides, so no pass/@output is needed.
TEST_CASE("ceir 15c: validate_ceir_frame catches CubeNeedsSquare (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_cube");
    FrameResourceDesc       c(&alloc);
    c.name.append("cubemap", 7);
    c.format  = gpu::FgImageFormat::RGBA8Unorm;
    c.width   = 256U;
    c.height  = 128U; // ⛔ NOT square
    c.kind_2d = gpu::FgImageKind::Cube;
    c.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(c));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // shape is a semantic, not structural, rule
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::CubeNeedsSquare);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::CubeNeedsSquare);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches VolumeNeedsDepth (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_volume0");
    FrameResourceDesc       v(&alloc);
    v.name.append("froxels", 7);
    v.format  = gpu::FgImageFormat::RGBA8Unorm;
    v.width   = 160U;
    v.height  = 90U;
    v.kind_2d = gpu::FgImageKind::Tex3D;
    v.depth   = 0U; // ⛔ a volume with no depth
    v.storage = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(v));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::VolumeNeedsDepth);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::VolumeNeedsDepth);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches BadMipCount (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_badmip");
    FrameResourceDesc       r(&alloc);
    r.name.append("bloom", 5);
    r.format  = gpu::FgImageFormat::RGBA8Unorm;
    r.width   = 256U;
    r.height  = 256U;
    r.mips    = 0U; // ⛔ 0 is not "full chain" — it must be an explicit level count
    r.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(r));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::BadMipCount);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::BadMipCount);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches LayersOutOfRange (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_layers");
    FrameResourceDesc       r(&alloc);
    r.name.append("atlas", 5);
    r.format  = gpu::FgImageFormat::RGBA8Unorm;
    r.width   = 256U;
    r.height  = 256U;
    r.layers  = 0U; // ⛔ a layer count outside [1, kFgMaxImageLayers]
    r.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(r));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::LayersOutOfRange);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::LayersOutOfRange);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches StructuredNeedsStride (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_stride0");
    FrameResourceDesc       r(&alloc);
    r.name.append("items", 5);
    r.kind   = FrameResourceKind::StructuredBuffer;
    r.count  = 100U;
    r.stride = 0U; // ⛔ elements with no size
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(r));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::StructuredNeedsStride);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::StructuredNeedsStride);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches StrideNotAligned (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_stride6");
    FrameResourceDesc       r(&alloc);
    r.name.append("items", 5);
    r.kind   = FrameResourceKind::StructuredBuffer;
    r.count  = 100U;
    r.stride = 6U; // ⛔ not a multiple of 4
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(r));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::StrideNotAligned);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::StrideNotAligned);
}

// CEIR-15c-1c-2 external-resource contract: an AccelerationStructure is host-built (a BLAS/TLAS needs scene geometry the
// frame must not depend on), so giving it a size means the author believed the graph allocates one. Per-import check.
TEST_CASE("ceir 15c: validate_ceir_frame catches AccelIsExternal (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_accel");
    FrameResourceDesc       as(&alloc);
    as.name.append("tlas", 4);
    as.kind       = FrameResourceKind::AccelerationStructure;
    as.size_bytes = 4096U; // ⛔ the graph never ALLOCATES an acceleration structure
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(as));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::AccelIsExternal);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::AccelIsExternal);
}

// CEIR-15c-1c-2 external-resource contract: a pass WRITING an ExternalTexture (the app owns its contents + update schedule).
// The pass is a raster.geometry WITH a draw list, so it dodges MissingDrawList — ExternalTextureIsReadOnly fires first.
TEST_CASE("ceir 15c: validate_ceir_frame catches ExternalTextureIsReadOnly (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_ext_write");
    FrameResourceDesc       ext(&alloc);
    ext.name.append("ui_atlas", 8);
    ext.kind    = FrameResourceKind::ExternalTexture;
    ext.format  = gpu::FgImageFormat::RGBA8Unorm;
    ext.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(ext));
    const crd::u32 dl = fb.add_draw_list("opaque");
    fb.draw_list_all(dl, "Transform");
    fb.draw_list_all(dl, "Mesh");
    fb.draw_list_policy(dl, FrameCullMode::Frustum, FrameSortMode::FrontToBack);
    const crd::u32 geo = fb.add_pass("overlay", "raster.geometry");
    fb.pass_draw_list(geo, "opaque");
    fb.pass_writes(geo, "ui_atlas"); // ⛔ writing an app-owned external texture

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::ExternalTextureIsReadOnly);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::ExternalTextureIsReadOnly);
}

// CEIR-15c-1c-2 NEW-IN-CEIR consistency (NO desc oracle — ceir.frame is a strict superset). Injected via set_attr; the
// third leg (op pointer + name round-trip) compensates for the missing oracle by locking the KIND and the enum→name switch.
TEST_CASE("ceir 15c: validate_ceir_frame catches KindLifetimeMismatch (frame_kind vs lifetime)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_klm");
    build_scene(fb);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const sc = find_graph_op(ctx, *m, StringView("resource.declare"), StringView("scene"));
    REQUIRE(sc != nullptr);
    ctx.set_attr(sc, "lifetime", ctx.attr_string(StringView("persistent"))); // ⛔ but frame_kind is still TransientImage
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::KindLifetimeMismatch);
    CHECK(d.op == sc); // points at the desynced declare
    CHECK(frame_semantic_kind_name(d.kind) == StringView("KindLifetimeMismatch")); // locks the enum→name switch
}

// The ABSENT-lifetime edition (the oldest scar in this file): a declare with frame_kind=PingPongImage and NO lifetime attr
// reads as lifetime="" — which kind_lifetime never returns, so the mismatch is CAUGHT, not silently normalized. Hand-built
// because the forward converter always sets lifetime (nothing to inject an absence into).
TEST_CASE("ceir 15c: KindLifetimeMismatch also catches an ABSENT lifetime", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    ceir::Context           ctx(&alloc);
    (void)ceir::arith::register_arith_ops(ctx);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::resource::register_resource_ops(ctx);
    (void)ceir::frame::register_dialect(ctx);
    const ceir::OpId decl_id  = ctx.intern_op("resource", "declare");
    const ceir::OpId graph_id = ctx.intern_op("frame", "graph");

    ceir::Module* const m   = ctx.create_module();
    ceir::Block*        top = m->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m->body()->append(top);
    }
    ceir::Operation* const fn = ceir::func::create_func(ctx, *m, "main", ceir::Visibility::Public, 0U);
    top->append(fn);
    ceir::Block* const   body = ceir::func::func_body_block(fn);
    ceir::Operation* const g  = ctx.create_operation(graph_id, {}, 0U, {}, 1U);
    body->append(g);
    ceir::Block* const rb = ctx.create_block(0U);
    g->region(0)->append(rb);
    ceir::Operation* const d = ctx.create_operation(decl_id, {}, 1U, ctx.type_image(ceir::ImageDim::Dim2D, ctx.type_f32()));
    ctx.set_attr(d, "name", ctx.attr_symbol(StringView("history")));
    ctx.set_attr(d, "frame_kind", ctx.attr_int(static_cast<crd::i64>(FrameResourceKind::PingPongImage)));
    // ⛔ deliberately NO lifetime attr
    rb->append(d);

    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None); // structurally fine — the desync is semantic
    const FrameSemanticDiag diag = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(diag.kind == FrameSemanticKind::KindLifetimeMismatch);
    CHECK(diag.op == d);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches UnknownDimension (garbage dimension int)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_unkdim");
    build_scene(fb);

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    ceir::Operation* const sc = find_graph_op(ctx, *m, StringView("resource.declare"), StringView("scene"));
    REQUIRE(sc != nullptr);
    ctx.set_attr(sc, "dimension", ctx.attr_int(99)); // ⛔ not a valid FgImageKind
    const FrameSemanticDiag d = validate_ceir_frame(ctx, *m, &alloc);
    CHECK(d.kind == FrameSemanticKind::UnknownDimension);
    CHECK(d.op == sc);
    CHECK(frame_semantic_kind_name(d.kind) == StringView("UnknownDimension"));
}

// CEIR-15c-1c-2 size rules: BadResourceSize (a TransientImage with neither absolute size nor scale — parse-time in the desc)
// and PersistentNeedsSize (a persistent image sized only by scale — its key must be stable across frames). Both oracle-backed.
TEST_CASE("ceir 15c: validate_ceir_frame catches BadResourceSize (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_nosize");
    FrameResourceDesc       r(&alloc);
    r.name.append("nosize", 6);
    r.kind   = FrameResourceKind::TransientImage;
    r.format = gpu::FgImageFormat::RGBA8Unorm; // ⛔ width=height=scale=0 — no size at all
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(r));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::BadResourceSize);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::BadResourceSize);
}

TEST_CASE("ceir 15c: validate_ceir_frame catches PersistentNeedsSize (oracle-agrees)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_persist");
    FrameResourceDesc       p(&alloc);
    p.name.append("accum", 5);
    p.kind    = FrameResourceKind::PersistentImage;
    p.format  = gpu::FgImageFormat::RGBA8Unorm;
    p.scale   = 1.0F; // ⛔ scale-only, no absolute size, and NOT resizable — the history key would move on resize
    p.sampled = true;
    fb.desc().resources.push_back(static_cast<FrameResourceDesc&&>(p));

    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(validate_ceir_frame(ctx, *m, &alloc).kind == FrameSemanticKind::PersistentNeedsSize);
    FrameGraphDesc desc(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc));
    CHECK(cook_verdict(desc, &alloc) == FrameCookError::PersistentNeedsSize);
}

// CEIR-15c-1c-2 (Fork C-2): the FULL RAF-12.3 param bag survives the converter round-trip. ⛔ Before this, to_ceir_frame
// carried only shader/kernel/technique/draw_list — the whole typed bag was dropped and the round-trip-identity gate
// false-greened (build_scene/build_taa set no params). This full-surface fixture (EVERY param type) re-locks it.
TEST_CASE("ceir 15c: the full RAF-12.3 param bag survives the round-trip (Fork C-2)", "[framecook][ceir][frame]")
{
    memory::MallocAllocator alloc;
    FrameGraphBuilder       fb(&alloc, "test_params");
    build_scene(fb);
    // inject EVERY param type on the `forward` pass (build_scene's 2nd): a String (view, generic — not a special name), a
    // Bool (load), a folded Float (clear_depth), a U32 (blend_count) + an Enum (blend slot), a Vec4 (clear_color), and an
    // AUTHORED Float (the [pass.params] `exposure_ev100`).
    FramePassDesc& p = fb.desc().passes[1];
    set_pass_str(p, StringView(pp::kView), StringView("main_view"));
    set_pass_flag(p, StringView(pp::kLoad), true);
    set_pass_f32(p, StringView(pp::kClearDepth), 0.5F);
    set_pass_u32(p, StringView(pp::kBlendCount), 1U);
    set_pass_enum(p, StringView(pp::kBlendSlot[0]), 1U);
    const float cc[4] = {0.1F, 0.2F, 0.3F, 0.4F};
    set_pass_vec4(p, StringView(pp::kClearColor), cc);
    set_pass_f32(p, StringView("exposure_ev100"), 2.5F);

    const String toml_a = emit_frame_toml(fb.desc(), &alloc);
    ceir::Context       ctx(&alloc);
    ceir::Module* const m = to_ceir_frame(fb.desc(), ctx);
    REQUIRE(m != nullptr);
    CHECK(ctx.find_frame_misuse(*m).kind == ceir::FrameMisuseKind::None);
    FrameGraphDesc desc2(&alloc);
    REQUIRE(from_ceir_frame(ctx, *m, &alloc, desc2));
    const String toml_b = emit_frame_toml(desc2, &alloc);
    CHECK(StringView(toml_a.data(), toml_a.size()) == StringView(toml_b.data(), toml_b.size())); // ⭐ the WHOLE bag round-trips

    // direct discrete-type checks on the reconstructed pass (the toml equality above covers the float VALUES).
    REQUIRE(desc2.passes.size() == 2U);
    const FramePassDesc& q = desc2.passes[1];
    CHECK(pass_str(q, StringView(pp::kView)) == StringView("main_view"));   // String (generic)
    CHECK(pass_flag(q, StringView(pp::kLoad)));                             // Bool
    CHECK(pass_u32(q, StringView(pp::kBlendCount), 0U) == 1U);              // U32
    CHECK(pass_u32(q, StringView(pp::kBlendSlot[0]), 0U) == 1U);            // Enum (read v[0] type-blind)
    CHECK(pass_has(q, StringView(pp::kClearColor)));                       // Vec4 present
    CHECK(pass_has(q, StringView("exposure_ev100")));                      // authored param present
}
