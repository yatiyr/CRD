// CEIR-31a-1b-ii (sec-142): the §142 AUDIO PROOF -- execute_audio_graph_ceir (the CEIR audio executor, walking a
// ceir.audio Module through the SHARED audio_kernels.hpp) is BIT-EXACT vs render_graph (the offline renderer, the
// deleting-reference oracle) on the SAME graph + the SAME source buffers. The CEIR side LOADS the committed
// assets/ceir/audio_source_gain_biquad_mix.ceir (31a-1a-ii proved it == build_audio_module); the oracle side hand-builds
// the matching AGRF node-for-node, edge-for-edge. ⛔ EDGE ORDER == the .ceir mix operand order (mix(%2,%3): biquad THEN
// source@b): render_graph sums a node's input edges in edge-list order and the CEIR executor sums operands in operand
// order -- the f32 sum is NON-ASSOCIATIVE, so (2->4) MUST precede (3->4) or the two paths diverge. Buffers are stereo with
// L != R so the per-channel biquad state is exercised independently; N=1000 (>= 4 render_graph blocks of 256, state
// carried); the compare is a BYTE compare (memcmp, not ==) so a -0.0 vs +0.0 divergence is caught. ⛔ NAMES STAY ASCII.

#include <crd/audio/ceir_audio_render.hpp>

#include <crd/audio/audio_graph.hpp>
#include <crd/audio/audio_kernels.hpp> // apply_delay (the direct-kernel gate)
#include <crd/audio/audio_resources.hpp>
#include <crd/ceir/attr.hpp> // AttrValue / AttrKind (the 31a-3 execution arm derives its reference params from the asset)
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/audio_ops.hpp>
#include <crd/ceir/id.hpp> // AttrId
#include <crd/ceir/ir.hpp> // Operation / Block (walking the parsed module)
#include <crd/ceir/parse.hpp>
#include <crd/ceir/type.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>   // std::exp / std::log10 / std::pow -- the INDEPENDENT compressor reference (NOT crd::math -- a tautology)
#include <cstring> // memcmp
#include <fstream> // slurp the committed .ceir (inline; NO CRD_REPO_DIR "." fallback -- the CMakeLists defines it)

using namespace crd;                    // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
// two NON-TRIVIAL stereo source buffers (L != R so the per-channel biquad recurrence is exercised independently), built
// ONCE -- the SAME bytes feed both the AGRF (positional) and the CEIR (by-name) bindings, so the inputs are identical.
void fill_a(containers::Array<crd::f32>& a, crd::i64 frames) // a decaying sine
{
    a.resize(static_cast<usize>(frames) * 2U, 0.0F);
    crd::f32 amp = 0.5F;
    for (crd::i64 f = 0; f < frames; ++f)
    {
        const crd::f32 l = amp * crd::math::sin(static_cast<crd::f32>(f) * 0.1F);
        a[static_cast<usize>(f) * 2U]      = l;
        a[static_cast<usize>(f) * 2U + 1U] = -0.7F * l;
        amp *= 0.999F;
    }
}
void fill_b(containers::Array<crd::f32>& b, crd::i64 frames) // an LCG noise burst in [-0.3, 0.3]
{
    b.resize(static_cast<usize>(frames) * 2U, 0.0F);
    crd::u32 s = 12345U;
    for (crd::i64 f = 0; f < frames; ++f)
    {
        s                = s * 1664525U + 1013904223U;
        const crd::f32 l = (static_cast<crd::f32>((s >> 8) & 0xFFFFFFU) / 16777215.0F) * 0.6F - 0.3F;
        b[static_cast<usize>(f) * 2U]      = l;
        b[static_cast<usize>(f) * 2U + 1U] = -0.7F * l;
    }
}

// slurp + parse a committed .ceir asset -- the cwd-luck REQUIRE(f.good()) in ONE place (CRD_REPO_DIR is the shared /mnt/d on
// WSL). `bytes` is CALLER-owned so it outlives the parse regardless of whether parse interns the source text.
ceir::ParseResult load_committed_ceir(ceir::Context& ctx, const char* path, containers::Array<char>& bytes)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    bytes.resize(static_cast<usize>(sz), '\0');
    f.read(bytes.data(), sz);
    return ceir::parse(ctx, StringView(bytes.data(), bytes.size()));
}

audio::AudioNodeRec node(audio::AudioNodeType type) // a default record of `type`
{
    audio::AudioNodeRec n;
    n.type = static_cast<crd::u8>(type);
    return n;
}

// the AGRF mirroring the committed .ceir node-for-node, edge-for-edge (the mix's input edges in operand order (2->4)
// then (3->4) == mix(%2,%3) -- irrelevant to a 2-input sum [commutative], kept for a faithful mirror).
audio::AudioGraphResource make_graph(crd::memory::IAllocator* alloc)
{
    audio::AudioGraphResource g(alloc);
    g.out_node = 4U;
    g.nodes.push_back(node(audio::AudioNodeType::Source)); // 0 = source@a
    g.nodes[0].name_off = g.intern("a");
    g.nodes.push_back(node(audio::AudioNodeType::Gain)); // 1 = gain{-6dB}
    g.nodes[1].gain_db = -6.0F;
    g.nodes.push_back(node(audio::AudioNodeType::Biquad)); // 2 = biquad{Lowpass,0.25,0.7071}
    g.nodes[2].filter = static_cast<crd::u8>(audio::BiquadType::Lowpass);
    g.nodes[2].cutoff = 0.25F;
    g.nodes[2].q      = 0.7071F;
    g.nodes.push_back(node(audio::AudioNodeType::Source)); // 3 = source@b
    g.nodes[3].name_off = g.intern("b");
    g.nodes.push_back(node(audio::AudioNodeType::Mix)); // 4 = mix(2, 3)
    g.edges.push_back({0U, 1U});
    g.edges.push_back({1U, 2U});
    g.edges.push_back({2U, 4U});
    g.edges.push_back({3U, 4U});
    return g;
}

// the AGRF for a 3-SOURCE MIX (a, b, c -> mix), the render_graph ORACLE for the 31z ruling-(1) order-sensitivity gate. The
// mix's input edges are in operand order (0->3, 1->3, 2->3) == mix(%0,%1,%2) in audio_mix3.ceir -- the f32 (non-associative)
// sum order render_graph and the CEIR executor MUST agree on.
audio::AudioGraphResource make_graph_mix3(crd::memory::IAllocator* alloc)
{
    audio::AudioGraphResource g(alloc);
    g.out_node = 3U;
    g.nodes.push_back(node(audio::AudioNodeType::Source)); // 0 = source@a
    g.nodes[0].name_off = g.intern("a");
    g.nodes.push_back(node(audio::AudioNodeType::Source)); // 1 = source@b
    g.nodes[1].name_off = g.intern("b");
    g.nodes.push_back(node(audio::AudioNodeType::Source)); // 2 = source@c
    g.nodes[2].name_off = g.intern("c");
    g.nodes.push_back(node(audio::AudioNodeType::Mix)); // 3 = mix(0, 1, 2)
    g.edges.push_back({0U, 3U});
    g.edges.push_back({1U, 3U});
    g.edges.push_back({2U, 3U});
    return g;
}
} // namespace

TEST_CASE("ceir 31a-1b-ii: execute_audio_graph_ceir is BIT-EXACT vs render_graph on the committed audio graph",
          "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    constexpr crd::i64            n_frames = 1000; // >= 4 render_graph blocks of 256 (biquad state carries across blocks)
    constexpr crd::i64            buf_len  = 1200; // >= n_frames so no one-shot silence tail masks a bug

    containers::Array<crd::f32> buf_a(&root);
    containers::Array<crd::f32> buf_b(&root);
    fill_a(buf_a, buf_len);
    fill_b(buf_b, buf_len);

    // ---- ORACLE: render_graph over a hand-built AGRF mirroring the .ceir node-for-node, edge-for-edge ----
    audio::AudioGraphResource g = make_graph(&root);

    audio::GraphSourceBinding binds[5] = {};
    binds[0].samples  = ConstSpan<crd::f32>(buf_a.data(), buf_a.size());
    binds[0].channels = 2U;
    binds[3].samples  = ConstSpan<crd::f32>(buf_b.data(), buf_b.size());
    binds[3].channels = 2U;

    containers::Array<crd::f32> out_oracle(&root);
    REQUIRE(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(binds, 5U), n_frames, out_oracle) == n_frames);

    // ---- CEIR: parse the committed .ceir + execute_audio_graph_ceir over the SAME buffers, bound BY NAME ----
    ceir::Context ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);

    containers::Array<char>  asset(&root);
    const ceir::ParseResult  pr =
        load_committed_ceir(ctx, CRD_REPO_DIR "/assets/ceir/audio_source_gain_biquad_mix.ceir", asset);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    audio::CeirSourceBinding cbinds[2];
    cbinds[0].name             = StringView("a");
    cbinds[0].binding.samples  = ConstSpan<crd::f32>(buf_a.data(), buf_a.size());
    cbinds[0].binding.channels = 2U;
    cbinds[1].name             = StringView("b");
    cbinds[1].binding.samples  = ConstSpan<crd::f32>(buf_b.data(), buf_b.size());
    cbinds[1].binding.channels = 2U;

    containers::Array<crd::f32> out_ceir(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *pr.module, ConstSpan<audio::CeirSourceBinding>(cbinds, 2U), n_frames,
                                            out_ceir) == n_frames);

    // ---- BIT-EXACT: byte-identical output (memcmp, not == -- catches -0.0 vs +0.0) ----
    REQUIRE(out_oracle.size() == static_cast<usize>(n_frames) * 2U);
    REQUIRE(out_ceir.size() == out_oracle.size());
    CHECK(std::memcmp(out_oracle.data(), out_ceir.data(), out_oracle.size() * sizeof(crd::f32)) == 0);

    // ---- NEGATIVE CONTROL (the gate has TEETH): the executor routes sources BY NAME, so binding buf_b to @a and buf_a
    // to @b MUST change the output (buf_a flows through gain->biquad, buf_b goes direct to the mix -- swapping them is a
    // real change). This proves the executor actually reads the `name` attr, not silently positional/ignored. NOTE: an
    // edge-ORDER swap is NOT a valid teeth test here -- the mix has 2 inputs and IEEE f32 add is COMMUTATIVE (a+b==b+a
    // bit-exactly; only ASSOCIATIVITY fails, needing 3+ operands), so the order is a genuine no-op for this fixture. ----
    audio::CeirSourceBinding rebound[2];
    rebound[0].name             = StringView("a"); // @a now bound to buf_b
    rebound[0].binding.samples  = ConstSpan<crd::f32>(buf_b.data(), buf_b.size());
    rebound[0].binding.channels = 2U;
    rebound[1].name             = StringView("b"); // @b now bound to buf_a
    rebound[1].binding.samples  = ConstSpan<crd::f32>(buf_a.data(), buf_a.size());
    rebound[1].binding.channels = 2U;
    containers::Array<crd::f32> out_rebound(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *pr.module, ConstSpan<audio::CeirSourceBinding>(rebound, 2U), n_frames,
                                            out_rebound) == n_frames);
    CHECK(std::memcmp(out_ceir.data(), out_rebound.data(), out_ceir.size() * sizeof(crd::f32)) != 0);
}

TEST_CASE("ceir 31z-(1): a 3-input mix is ORDER-SENSITIVE -- CEIR matches render_graph in the authored operand order, and a "
          "PERMUTED order differs (f32 sum non-associativity)",
          "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    constexpr crd::i64            n = 64;

    // ⛔ constant buffers chosen so the 3-way f32 sum is NON-ASSOCIATIVE: (va+vb)+vc collapses (vb lost under va), but
    // (va+vc)+vb recovers vb. The REQUIRE precondition makes the tooth non-inert (a triple that happened to associate = no
    // test). Neither result is the ambiguous -0.0/+0.0 boundary (authored 0.0, permuted 1.0), sidestepping the sign-of-zero scar.
    constexpr crd::f32 va           = 1e8F;
    constexpr crd::f32 vb           = 1.0F;
    constexpr crd::f32 vc           = -1e8F;
    const crd::f32     authored_sum = (va + vb) + vc; // = 0.0F  (1e8 + 1 rounds to 1e8, then + (-1e8) = 0)
    const crd::f32     permuted_sum = (va + vc) + vb; // = 1.0F  (1e8 + (-1e8) = 0, then + 1 = 1)
    REQUIRE(authored_sum != permuted_sum);            // the non-associativity precondition -- else the order tooth is inert

    containers::Array<crd::f32> buf_a(&root);
    containers::Array<crd::f32> buf_b(&root);
    containers::Array<crd::f32> buf_c(&root);
    buf_a.resize(static_cast<usize>(n) * 2U, va);
    buf_b.resize(static_cast<usize>(n) * 2U, vb);
    buf_c.resize(static_cast<usize>(n) * 2U, vc);

    // ---- ORACLE: render_graph over the 3-source-mix AGRF (edges in operand order a, b, c) ----
    audio::AudioGraphResource g = make_graph_mix3(&root);
    // ⛔ render_graph needs ONE binding PER NODE (bindings.size() >= node count); the mix node's binding is empty/ignored.
    audio::GraphSourceBinding binds[4] = {};
    binds[0].samples                   = ConstSpan<crd::f32>(buf_a.data(), buf_a.size());
    binds[0].channels                  = 2U;
    binds[1].samples                   = ConstSpan<crd::f32>(buf_b.data(), buf_b.size());
    binds[1].channels                  = 2U;
    binds[2].samples                   = ConstSpan<crd::f32>(buf_c.data(), buf_c.size());
    binds[2].channels                  = 2U;
    containers::Array<crd::f32> out_oracle(&root);
    REQUIRE(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(binds, 4U), n, out_oracle) == n);

    // ---- CEIR: the COMMITTED audio_mix3.ceir (mix(%0,%1,%2)) over the same buffers, bound by name ----
    ceir::Context ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);
    containers::Array<char> asset(&root);
    const ceir::ParseResult pr = load_committed_ceir(ctx, CRD_REPO_DIR "/assets/ceir/audio_mix3.ceir", asset);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    audio::CeirSourceBinding cb[3];
    cb[0].name             = StringView("a");
    cb[0].binding.samples  = ConstSpan<crd::f32>(buf_a.data(), buf_a.size());
    cb[0].binding.channels = 2U;
    cb[1].name             = StringView("b");
    cb[1].binding.samples  = ConstSpan<crd::f32>(buf_b.data(), buf_b.size());
    cb[1].binding.channels = 2U;
    cb[2].name             = StringView("c");
    cb[2].binding.samples  = ConstSpan<crd::f32>(buf_c.data(), buf_c.size());
    cb[2].binding.channels = 2U;
    containers::Array<crd::f32> out_ceir(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *pr.module, ConstSpan<audio::CeirSourceBinding>(cb, 3U), n, out_ceir) == n);

    // ---- BIT-EXACT vs render_graph in the AUTHORED order (the operand-order == edge-order contract) ----
    REQUIRE(out_ceir.size() == out_oracle.size());
    CHECK(std::memcmp(out_oracle.data(), out_ceir.data(), out_oracle.size() * sizeof(crd::f32)) == 0);
    CHECK(out_ceir[0] == authored_sum); // a positive pin (= 0.0) the memcmp alone would not give

    // ---- PERMUTED CEIR MODULE (mix(%0,%2,%1) == a, c, b) -- the ONLY change is the mix operand ORDER; the output DIFFERS ----
    ceir::Module* const mperm = ctx.create_module();
    ceir::Block*        top   = mperm->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        mperm->body()->append(top);
    }
    ceir::Operation* const fn = ceir::func::create_func(ctx, *mperm, "main", ceir::Visibility::Public, 0U);
    top->append(fn);
    ceir::Block* const fb      = ceir::func::func_body_block(fn);
    const ceir::TypeId dims[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(2U)};
    const ceir::TypeId tt      = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(dims, 2U)));
    ceir::Operation* const sa =
        ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("a")), ctx.attr_int(0), ctx.attr_int(0), tt);
    fb->append(sa);
    ceir::Operation* const sb =
        ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("b")), ctx.attr_int(0), ctx.attr_int(0), tt);
    fb->append(sb);
    ceir::Operation* const sc =
        ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("c")), ctx.attr_int(0), ctx.attr_int(0), tt);
    fb->append(sc);
    ceir::Value* const     perm_in[3] = {sa->result(0U), sc->result(0U), sb->result(0U)}; // a, c, b -- b and c SWAPPED
    ceir::Operation* const pmix =
        ctx.create_operation(ceir::audio::mix_kind(ctx), ConstSpan<ceir::Value*>(perm_in, 3U), 1U, tt);
    fb->append(pmix);
    containers::Array<crd::f32> out_perm(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *mperm, ConstSpan<audio::CeirSourceBinding>(cb, 3U), n, out_perm) == n);
    REQUIRE(out_perm.size() == out_ceir.size());
    CHECK(std::memcmp(out_ceir.data(), out_perm.data(), out_ceir.size() * sizeof(crd::f32)) != 0); // ORDER matters
    CHECK(out_perm[0] == permuted_sum);                                                            // = 1.0, the (va+vc)+vb value
}

TEST_CASE("ceir 31z-(2): the executor REFUSES a delay-headed feedback cycle (no graph-feedback oracle) while the straight-line "
          "delay chain still renders",
          "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    constexpr crd::i64            n = 64;
    containers::Array<crd::f32>   in_buf(&root);
    fill_a(in_buf, n);

    ceir::Context ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);
    const ceir::TypeId dims[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(2U)};
    const ceir::TypeId tt      = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(dims, 2U)));

    audio::CeirSourceBinding cb[1];
    cb[0].name             = StringView("s");
    cb[0].binding.samples  = ConstSpan<crd::f32>(in_buf.data(), in_buf.size());
    cb[0].binding.channels = 2U;

    // helper: a fresh main{} func with a body block.
    const auto new_main = [&](ceir::Module*& m) -> ceir::Block* {
        m                       = ctx.create_module();
        ceir::Block* top        = m->body()->first_block();
        if (top == nullptr)
        {
            top = ctx.create_block(0U);
            m->body()->append(top);
        }
        ceir::Operation* const fn = ceir::func::create_func(ctx, *m, "main", ceir::Visibility::Public, 0U);
        top->append(fn);
        return ceir::func::func_body_block(fn);
    };

    // (a) a delay-headed feedback echo %s=source; %d=delay(%m); %m=mix(%s,%d). The 5d verifier ACCEPTS this (delay's
    // StateEdge back-edge -- proven in test_audio_gate 31z-(2)); the single-pass EXECUTOR REFUSES it (its input bus is not
    // yet computed; render_graph refuses cycles too). execute -> 0.
    {
        ceir::Module*          m = nullptr;
        ceir::Block* const     fb = new_main(m);
        ceir::Operation* const s =
            ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("s")), ctx.attr_int(0), ctx.attr_int(0), tt);
        fb->append(s);
        ceir::Operation* const dly = ceir::audio::build_delay(ctx, s->result(0U), ctx.attr_int(8), tt); // temp operand = s
        fb->append(dly);
        ceir::Value* const     mix_in[2] = {s->result(0U), dly->result(0U)};
        ceir::Operation* const mix =
            ctx.create_operation(ceir::audio::mix_kind(ctx), ConstSpan<ceir::Value*>(mix_in, 2U), 1U, tt);
        fb->append(mix);
        dly->set_operand(0U, mix->result(0U)); // the BACK-EDGE: delay reads mix (defined LATER)
        containers::Array<crd::f32> out(&root);
        CHECK(audio::execute_audio_graph_ceir(ctx, *m, ConstSpan<audio::CeirSourceBinding>(cb, 1U), n, out) == 0); // REFUSED
    }
    // (b) the straight-line delay chain %s=source; %d=delay(%s) STILL renders (the reject is back-edge-specific, not "delay broke").
    {
        ceir::Module*          m  = nullptr;
        ceir::Block* const     fb = new_main(m);
        ceir::Operation* const s =
            ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("s")), ctx.attr_int(0), ctx.attr_int(0), tt);
        fb->append(s);
        fb->append(ceir::audio::build_delay(ctx, s->result(0U), ctx.attr_int(8), tt));
        containers::Array<crd::f32> out(&root);
        CHECK(audio::execute_audio_graph_ceir(ctx, *m, ConstSpan<audio::CeirSourceBinding>(cb, 1U), n, out) == n); // RENDERS
    }
}

TEST_CASE("ceir 31z-(5): the executor validates `frames` against a PINNED (Static) frame dim by EQUALITY; a Dynamic frame dim "
          "accepts any block size",
          "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    ceir::Context                 ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);

    containers::Array<crd::f32> in_buf(&root);
    fill_a(in_buf, 256); // >= either block size below (128 frames need 256 samples)
    audio::CeirSourceBinding cb[1];
    cb[0].name             = StringView("s");
    cb[0].binding.samples  = ConstSpan<crd::f32>(in_buf.data(), in_buf.size());
    cb[0].binding.channels = 2U;

    // a source@s -> gain{0} module whose values carry frame-dim type `tt` (so the sink gain's result type pins dim0).
    const auto build = [&](ceir::TypeId tt) -> ceir::Module* {
        ceir::Module* const m = ctx.create_module();
        ceir::Block*        top = m->body()->first_block();
        if (top == nullptr)
        {
            top = ctx.create_block(0U);
            m->body()->append(top);
        }
        ceir::Operation* const fn = ceir::func::create_func(ctx, *m, "main", ceir::Visibility::Public, 0U);
        top->append(fn);
        ceir::Block* const     fb = ceir::func::func_body_block(fn);
        ceir::Operation* const s =
            ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("s")), ctx.attr_int(0), ctx.attr_int(0), tt);
        fb->append(s);
        fb->append(ceir::audio::build_gain(ctx, s->result(0U), ctx.attr_float(0.0), tt));
        return m;
    };

    // STATIC [64, 2]: `frames` must EQUAL 64.
    const ceir::TypeId s64[2] = {ctx.type_dim_static(64U), ctx.type_dim_static(2U)};
    const ceir::TypeId t64    = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(s64, 2U)));
    containers::Array<crd::f32> o128(&root);
    CHECK(audio::execute_audio_graph_ceir(ctx, *build(t64), ConstSpan<audio::CeirSourceBinding>(cb, 1U), 128, o128) == 0); // 128 != 64 -> REJECT
    containers::Array<crd::f32> o64(&root);
    CHECK(audio::execute_audio_graph_ceir(ctx, *build(t64), ConstSpan<audio::CeirSourceBinding>(cb, 1U), 64, o64) == 64); // 64 == 64 -> RENDER (equality, not >=)

    // DYNAMIC [dyn, 2] (the committed assets' type): any block size renders.
    const ceir::TypeId sdyn[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(2U)};
    const ceir::TypeId tdyn    = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(sdyn, 2U)));
    containers::Array<crd::f32> odyn(&root);
    CHECK(audio::execute_audio_graph_ceir(ctx, *build(tdyn), ConstSpan<audio::CeirSourceBinding>(cb, 1U), 128, odyn) == 128); // any block OK
}

TEST_CASE("ceir 31a-2a: apply_delay shifts bit-exact and the executor dispatches audio.delay", "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    constexpr crd::i64            n = 64;

    containers::Array<crd::f32> in_buf(&root);
    fill_a(in_buf, n); // a decaying sine, L != R

    // ---- DIRECT KERNEL: apply_delay(bus, n, D) == the input shifted by D (+0.0F before), for D = 0 (identity, catches an
    // off-by-one), D = 17 (mid), D = n+10 (>= n, all-zero output). memcmp so a -0.0 vs +0.0 in the pre-roll is caught. ----
    const crd::i64 ds[4] = {0, 17, n - 1, n + 10}; // D = 0 identity, mid, LAST-frame (off-by-one boundary), >= n silence
    for (crd::i64 di = 0; di < 4; ++di)
    {
        const crd::i64              d = ds[di];
        containers::Array<crd::f32> bus(&root);
        bus.resize(static_cast<usize>(n) * 2U, 0.0F);
        for (usize k = 0; k < bus.size(); ++k) { bus[k] = in_buf[k]; }
        audio::apply_delay(bus.data(), n, d);
        for (crd::i64 f = 0; f < n; ++f)
        {
            for (crd::i64 c = 0; c < 2; ++c)
            {
                const crd::f32 expect = (f >= d) ? in_buf[static_cast<usize>((f - d) * 2 + c)] : 0.0F;
                const crd::f32 got    = bus[static_cast<usize>(f * 2 + c)];
                CHECK(std::memcmp(&expect, &got, sizeof(crd::f32)) == 0);
            }
        }
    }

    // ---- EXECUTOR DISPATCH: a source@s -> delay{17} module renders == apply_delay applied to the source samples (proving
    // the executor routes audio.delay to the kernel). Built with the ceir builders -- a KERNEL gate, not a committed asset.
    ceir::Context ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);

    ceir::Module* const m   = ctx.create_module();
    ceir::Block*        top = m->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m->body()->append(top);
    }
    ceir::Operation* const fn = ceir::func::create_func(ctx, *m, "main", ceir::Visibility::Public, 0U);
    top->append(fn);
    ceir::Block* const fb = ceir::func::func_body_block(fn);

    const ceir::TypeId dims[2] = {ctx.type_dim_static(static_cast<crd::u32>(n)), ctx.type_dim_static(2U)};
    const ceir::TypeId tt      = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(dims, 2U)));

    ceir::Operation* const s =
        ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("s")), ctx.attr_int(0), ctx.attr_int(0), tt);
    fb->append(s);
    constexpr crd::i64 exec_d = 17;
    fb->append(ceir::audio::build_delay(ctx, s->result(0U), ctx.attr_int(exec_d), tt));

    audio::CeirSourceBinding cb[1];
    cb[0].name             = StringView("s");
    cb[0].binding.samples  = ConstSpan<crd::f32>(in_buf.data(), in_buf.size());
    cb[0].binding.channels = 2U;
    containers::Array<crd::f32> out(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *m, ConstSpan<audio::CeirSourceBinding>(cb, 1U), n, out) == n);

    // expected: MIRROR the executor's dataflow -- source rendered (apply_source SUMS into a zeroed bus, the += that turns
    // an input -0.0F into +0.0F, exactly as render_graph does), then delayed. ⛔ a copy-based reference (expect[k]=in_buf[k])
    // WRONGLY preserves the source buffer's -0.0F and diverges on the pre-roll boundary -- the -0.0 vs +0.0 case memcmp is
    // for. (apply_delay's numerics are the direct gate above; apply_source's are 31a-1b-ii; this proves the WIRING.)
    containers::Array<crd::f32> expect(&root);
    expect.resize(static_cast<usize>(n) * 2U, 0.0F);
    audio::apply_source(expect.data(), 0, n, cb[0].binding, 0, false);
    audio::apply_delay(expect.data(), n, exec_d);
    REQUIRE(out.size() == expect.size());
    CHECK(std::memcmp(out.data(), expect.data(), out.size() * sizeof(crd::f32)) == 0);
}

TEST_CASE("ceir 31a-2b: apply_compressor tracks a two-segment step within tolerance of an independent reference",
          "[audio][ceir]")
{
    // A feed-forward peak compressor on a STEP input has a CLOSED FORM for the envelope, computed here with std:: (exp/
    // log10/pow) -- INDEPENDENT of crd::math (the kernel's libm), so the tolerance catches a wrong recurrence, not a
    // tautology. Two segments in ONE buffer + ONE envelope state exercise: (seg 1) ATTACK rising through the threshold
    // (gain 1 below, < 1 above -- env[n] = A*(1 - a^(n+1))); (seg 2) the attack->RELEASE switch when the input drops to a
    // NONZERO B below threshold (a step DOWN to zero would prove nothing -- out = 0 regardless of gain), then RELEASE
    // decaying from env_boundary toward B and CROSSING below the threshold (gain -> 1) -- env[N+k] = B+(env_N-B)*r^(k+1).
    // ⛔ params are f32 (the kernel narrows them); the reference uses the SAME f32-narrowed values. The divergence is
    // DOMINATED by f32 OUTPUT quantization -- the audio bus is f32, so `got` carries ~1 ULP of f32 (~6e-8 relative); the
    // f64 libm difference (crd::math vs std:: through exp/log10/pow) + the recurrence-vs-closed-form accumulation are ~1e-13,
    // negligible beneath it. Tolerance 1e-5 RELATIVE (the contract, robust) with a 1e-3 absolute floor (so near-zero
    // pre-threshold samples don't divide by ~0); a 2e-7 tripwire pins the observed ~6e-8 f32 floor (a regression catch).
    memory::GrowableTlsfAllocator root;

    constexpr crd::u32 sr        = 48000U;
    constexpr crd::f32 thr_db    = -20.0F; // threshold_lin = 0.1
    constexpr crd::f32 ratio     = 4.0F;
    constexpr crd::f32 atk_ms    = 5.0F;  // tau ~ 240 samples
    constexpr crd::f32 rel_ms    = 20.0F; // tau ~ 960 samples
    constexpr crd::f32 step_a    = 0.5F;  // above threshold (-6 dB): attack compresses
    constexpr crd::f32 step_b    = 0.05F; // below threshold (0.1), NONZERO: release path with a live gain
    constexpr crd::i64 n_attack  = 1200;  // ~5 tau: env rises close to A, well above threshold
    constexpr crd::i64 n_release = 3000;  // long enough for release to carry env from ~A back BELOW the threshold
    constexpr crd::i64 total     = n_attack + n_release;

    // the step bus -- the two channels carry DIFFERENT magnitudes and the PEAK is the NEGATIVE channel 1 (L = 0.5*v on
    // ch0, R = -v on ch1), so d = max(|L|,|R|) = v is reproduced ONLY by a stereo-LINKED peak-with-abs detector: an
    // L-only detector gets 0.5v, an R-only-without-abs gets -v (env goes negative), a two-channel mean gets a wrong
    // magnitude -- each diverges. The per-channel gain is the SAME (one linked envelope), applied to each channel's own
    // sample (out_c = scale[c]*v*gain). ⛔ this is the identity-not-category teeth for the "stereo-linked peak" contract.
    constexpr crd::f64 scale[2] = {0.5, -1.0}; // ch0 = +0.5*v (smaller), ch1 = -1.0*v (the larger, NEGATIVE peak)
    containers::Array<crd::f32> bus(&root);
    bus.resize(static_cast<usize>(total) * 2U, 0.0F);
    for (crd::i64 f = 0; f < total; ++f)
    {
        const crd::f32 v = (f < n_attack) ? step_a : step_b;
        bus[static_cast<usize>(f) * 2U]      = static_cast<crd::f32>(scale[0]) * v;
        bus[static_cast<usize>(f) * 2U + 1U] = static_cast<crd::f32>(scale[1]) * v;
    }

    crd::f64 env = 0.0;
    audio::apply_compressor(bus.data(), total, thr_db, ratio, atk_ms, rel_ms, sr, env);

    // ---- INDEPENDENT reference (std::, f64 over the f32-narrowed params): the envelope closed form + the same static curve.
    const crd::f64 srf   = static_cast<crd::f64>(sr);
    const crd::f64 a      = std::exp(-1.0 / (static_cast<crd::f64>(atk_ms) * 1.0e-3 * srf));
    const crd::f64 r      = std::exp(-1.0 / (static_cast<crd::f64>(rel_ms) * 1.0e-3 * srf));
    const crd::f64 thr    = static_cast<crd::f64>(thr_db);
    const crd::f64 slope  = 1.0 / static_cast<crd::f64>(ratio) - 1.0;
    const crd::f64 av     = static_cast<crd::f64>(step_a);
    const crd::f64 bv     = static_cast<crd::f64>(step_b);
    const crd::f64 env_bd = av * (1.0 - std::pow(a, static_cast<crd::f64>(n_attack))); // env at the last attack sample

    crd::u32 above_thr     = 0; // # samples the reference gain reduces (attack + early release) -- the gain path IS exercised
    crd::u32 unity_release = 0; // # RELEASE samples below threshold (gain unity) -- pins that release CROSSES the threshold
    crd::f64 max_dev_rel   = 0.0; // the worst observed relative deviation (vs the 2e-7 f32-quantization tripwire below)
    for (crd::i64 f = 0; f < total; ++f)
    {
        crd::f64 env_f = 0.0;
        crd::f64 d     = 0.0;
        if (f < n_attack)
        {
            env_f = av * (1.0 - std::pow(a, static_cast<crd::f64>(f + 1)));
            d     = av;
        }
        else
        {
            const crd::i64 k = f - n_attack;
            env_f            = bv + (env_bd - bv) * std::pow(r, static_cast<crd::f64>(k + 1));
            d                = bv;
        }
        const crd::f64 env_db  = 20.0 * std::log10((env_f > 1.0e-9) ? env_f : 1.0e-9);
        const crd::f64 over    = env_db - thr;
        const crd::f64 gain_db = (over > 0.0) ? over * slope : 0.0;
        const crd::f64 gain    = std::pow(10.0, gain_db / 20.0);
        if (over > 0.0) { ++above_thr; }
        else if (f >= n_attack) { ++unity_release; }

        for (crd::i64 c = 0; c < 2; ++c)
        {
            const crd::f64 ref   = scale[c] * d * gain;
            const crd::f64 floor = (std::fabs(ref) > 1.0e-3) ? std::fabs(ref) : 1.0e-3; // abs floor (ch1's ref is negative)
            const crd::f64 got   = static_cast<crd::f64>(bus[static_cast<usize>(f * 2 + c)]);
            const crd::f64 rel   = std::fabs(got - ref) / floor;
            CHECK(rel <= 1.0e-5); // the stated CONTRACT tolerance (two libms + recurrence-vs-closed-form, f64)
            if (rel > max_dev_rel) { max_dev_rel = rel; }
        }
    }
    // the step exercises BOTH regimes: compression (attack + early release above threshold) AND a release CROSSING below
    // the threshold back to unity -- else the tolerance gate could pass on a flat line. The observed deviation (~6e-8) is
    // the f32 OUTPUT-quantization floor (1 ULP of the f32 bus), FAR under the 1e-5 contract; the 2e-7 tripwire sits just
    // above that floor -- ~50x tighter than the contract, so a real numerics regression trips it while f32 rounding does not.
    // ⛔ what this GATES: a wrong recurrence or static curve (e.g. swapped envelope coefficients, or a non-linked / no-abs
    // detector -- VERIFIED to blow past 2e-7 by temporarily hacking d = |L|, which the asymmetric-negative-channel fixture
    // catches). What it does NOT gate: the libm-level f64 difference (crd::math vs std:: ~1e-16), which is BELOW the f32
    // storage floor and irrelevant to what the executor produces.
    CHECK(above_thr > 0U);
    CHECK(unity_release > 0U);
    CHECK(max_dev_rel < 2.0e-7);
}

TEST_CASE("ceir 31a-2b-ii: the executor dispatches audio.compressor and reads the graph sample_rate", "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    constexpr crd::i64            n = 500; // covers the attack ramp at both 48k (tau ~240) and 96k (tau ~480)

    // an asymmetric-NEGATIVE-channel source (L = +0.25 ch0, R = -0.50 ch1) so the executor path inherits the stereo-LINKED
    // peak-with-abs teeth for FREE (d = max(|L|,|R|) = 0.5 from the NEGATIVE channel); 0.5 (-6 dB) is above the -20 dB
    // threshold so the compressor actively reduces gain -- ELSE the sample_rate coeff swap below would not move the output.
    containers::Array<crd::f32> src(&root);
    src.resize(static_cast<usize>(n) * 2U, 0.0F);
    for (crd::i64 f = 0; f < n; ++f)
    {
        src[static_cast<usize>(f) * 2U]      = 0.25F;  // L = +0.25 (the smaller magnitude)
        src[static_cast<usize>(f) * 2U + 1U] = -0.50F; // R = -0.50 (the larger, NEGATIVE peak -- needs abs AND the linked max)
    }

    ceir::Context ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);

    // main{sample_rate=sr} { %0=source@s; %1=compressor(%0){-20,4,5,20} } -- the sample_rate rides the func, NOT the exec call.
    auto build = [&](crd::i64 sr) -> ceir::Module* {
        ceir::Module* const m   = ctx.create_module();
        ceir::Block*        top = m->body()->first_block();
        if (top == nullptr)
        {
            top = ctx.create_block(0U);
            m->body()->append(top);
        }
        ceir::Operation* const fn = ceir::func::create_func(ctx, *m, "main", ceir::Visibility::Public, 0U);
        top->append(fn);
        ctx.set_attr(fn, "sample_rate", ctx.attr_int(sr));
        ceir::Block* const fb      = ceir::func::func_body_block(fn);
        const ceir::TypeId dims[2] = {ctx.type_dim_static(static_cast<crd::u32>(n)), ctx.type_dim_static(2U)};
        const ceir::TypeId tt      = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(dims, 2U)));
        ceir::Operation* const s =
            ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("s")), ctx.attr_int(0), ctx.attr_int(0), tt);
        fb->append(s);
        fb->append(ceir::audio::build_compressor(ctx, s->result(0U), ctx.attr_float(-20.0), ctx.attr_float(4.0),
                                                 ctx.attr_float(5.0), ctx.attr_float(20.0), tt));
        return m;
    };

    audio::CeirSourceBinding cb[1];
    cb[0].name             = StringView("s");
    cb[0].binding.samples  = ConstSpan<crd::f32>(src.data(), src.size());
    cb[0].binding.channels = 2U;

    // ---- DISPATCH + memcmp vs the kernel: expect = apply_compressor(apply_source(zeroed), ..., sr=48000, env=0). BOTH sides
    // call the IDENTICAL apply_compressor, so this is BIT-EXACT (memcmp) -- it proves ROUTING + the sr plumbing (the kernel
    // numerics are 31a-2b-i's tolerance gate above). Same shape as the 31a-2a delay dispatch gate.
    ceir::Module* const         m48 = build(48000);
    containers::Array<crd::f32> out48(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *m48, ConstSpan<audio::CeirSourceBinding>(cb, 1U), n, out48) == n);

    containers::Array<crd::f32> expect(&root);
    expect.resize(static_cast<usize>(n) * 2U, 0.0F);
    audio::apply_source(expect.data(), 0, n, cb[0].binding, 0, false);
    crd::f64 env = 0.0;
    audio::apply_compressor(expect.data(), n, -20.0F, 4.0F, 5.0F, 20.0F, 48000U, env);
    REQUIRE(out48.size() == expect.size());
    CHECK(std::memcmp(out48.data(), expect.data(), out48.size() * sizeof(crd::f32)) == 0);

    // ---- sr TEETH: the SAME module at 96000 MUST differ (coeffs = exp(-1/(ms*1e-3*sr)) change with sr) -- proves the
    // executor READS the func sample_rate, not a hardcoded 48000 (the fixture's rate). The by-NAME-binding-swap teeth
    // pattern applied to sample_rate: without it a hardcoded rate false-greens the memcmp above.
    ceir::Module* const         m96 = build(96000);
    containers::Array<crd::f32> out96(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *m96, ConstSpan<audio::CeirSourceBinding>(cb, 1U), n, out96) == n);
    REQUIRE(out96.size() == out48.size());
    CHECK(std::memcmp(out48.data(), out96.data(), out48.size() * sizeof(crd::f32)) != 0);

    // ---- GRACEFUL REJECT: a corrupt sample_rate (0) makes the executor REFUSE (return 0), never silently default to
    // 48000 (the graceful-reject scar). A future "helpful" default-on-corrupt edit would pass every arm above; this
    // catches it. (find_audio_misuse also flags it at 31a-2b-ii's #650; the executor rejects at the read site regardless.)
    ceir::Module* const         m0 = build(0);
    containers::Array<crd::f32> out0(&root);
    CHECK(audio::execute_audio_graph_ceir(ctx, *m0, ConstSpan<audio::CeirSourceBinding>(cb, 1U), n, out0) == 0);
}

TEST_CASE("ceir 31a-3: execute_audio_graph_ceir walks the committed full-chain asset bit-exact vs the composed kernels",
          "[audio][ceir]")
{
    memory::GrowableTlsfAllocator root;
    // ⛔ n >= the asset type's [256,2] shape decl (a KNOWN non-check: the executor uses the `frames` call param, NOT the
    // Tensor shape -- noted in the tracker, not fixed this tick). buf_len >= n so no one-shot silence tail masks a bug.
    constexpr crd::i64 n       = 1000;
    constexpr crd::i64 buf_len = 1200;

    containers::Array<crd::f32> buf_a(&root);
    containers::Array<crd::f32> buf_b(&root);
    fill_a(buf_a, buf_len);
    fill_b(buf_b, buf_len);

    // ---- CEIR: load the COMMITTED audio_full_chain.ceir + execute over @a,@b bound BY NAME ----
    ceir::Context ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);

    containers::Array<char>  asset(&root);
    const ceir::ParseResult  pr = load_committed_ceir(ctx, CRD_REPO_DIR "/assets/ceir/audio_full_chain.ceir", asset);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    audio::CeirSourceBinding cbinds[2];
    cbinds[0].name             = StringView("a");
    cbinds[0].binding.samples  = ConstSpan<crd::f32>(buf_a.data(), buf_a.size());
    cbinds[0].binding.channels = 2U;
    cbinds[1].name             = StringView("b");
    cbinds[1].binding.samples  = ConstSpan<crd::f32>(buf_b.data(), buf_b.size());
    cbinds[1].binding.channels = 2U;

    containers::Array<crd::f32> out(&root);
    REQUIRE(audio::execute_audio_graph_ceir(ctx, *pr.module, ConstSpan<audio::CeirSourceBinding>(cbinds, 2U), n, out) == n);

    // ⛔ derive the reference PARAMS from the PARSED asset (NOT literals): an asset param edit (e.g. delay 64->32) that
    // regenerates the oracle would leave a literal reference testing stale values -- memcmp red LOOKING like an executor
    // bug. Reading what the executor reads makes this arm honestly test THE WALK for whatever the asset says. The TOPOLOGY
    // (source@a->chain, source@b->mix) is the fixture contract, caught by #651's anti-drift + 7-op completeness arms.
    ceir::Operation* pf = nullptr;
    for (ceir::Operation* op = pr.module->body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == StringView("func.func")) { pf = op; break; }
    }
    REQUIRE(pf != nullptr);
    ceir::Operation* op_gain   = nullptr;
    ceir::Operation* op_biquad = nullptr;
    ceir::Operation* op_delay  = nullptr;
    ceir::Operation* op_comp   = nullptr;
    for (ceir::Operation* op = pf->region(0)->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        const StringView nm = ctx.op_name(op->kind());
        if (nm == StringView("audio.gain")) { op_gain = op; }
        else if (nm == StringView("audio.biquad")) { op_biquad = op; }
        else if (nm == StringView("audio.delay")) { op_delay = op; }
        else if (nm == StringView("audio.compressor")) { op_comp = op; }
    }
    REQUIRE(op_gain != nullptr);
    REQUIRE(op_biquad != nullptr);
    REQUIRE(op_delay != nullptr);
    REQUIRE(op_comp != nullptr);
    auto rf = [&](const ceir::Operation* op, const char* name) -> crd::f32 {
        const ceir::AttrId a = op->attr(name);
        REQUIRE(a.valid());
        return static_cast<crd::f32>(ctx.attr_value(a).as_float());
    };
    auto ri = [&](const ceir::Operation* op, const char* name) -> crd::i64 {
        const ceir::AttrId a = op->attr(name);
        REQUIRE(a.valid());
        return ctx.attr_value(a).i;
    };
    const crd::f32 a_gain_db = rf(op_gain, "gain_db");
    const crd::u8  a_filter  = static_cast<crd::u8>(ri(op_biquad, "filter"));
    const crd::f32 a_cutoff  = rf(op_biquad, "cutoff");
    const crd::f32 a_q       = rf(op_biquad, "q");
    const crd::i64 a_delay   = ri(op_delay, "delay_frames");
    const crd::f32 a_thr     = rf(op_comp, "threshold_db");
    const crd::f32 a_ratio   = rf(op_comp, "ratio");
    const crd::f32 a_attack  = rf(op_comp, "attack_ms");
    const crd::f32 a_release = rf(op_comp, "release_ms");
    const crd::u32 a_sr      = static_cast<crd::u32>(ri(pf, "sample_rate"));

    // ---- COMPOSED-KERNEL REFERENCE: mirror the executor's per-node "zeroed bus, += each input, apply op" EXACTLY -- the
    // sum-into-zeroed normalizes -0.0F at EVERY node (the sign-of-zero scar), so do NOT apply the chain in-place. Each stage
    // is SINGLE-PASS over all n frames (biquad z=0, compressor env=0, both started here -- NOT 256-blocked). ⛔ this proves
    // the executor walks the 6-node committed TOPOLOGY (operand sums, node order, sink, the sr read from the func attr) --
    // NOT numerics (those are 31a-2b-i's tolerance gate + 31a-1b-ii); both sides call the IDENTICAL kernels, hence memcmp.
    const usize span     = static_cast<usize>(n) * 2U;
    auto        zeroed   = [&](containers::Array<crd::f32>& a) { a.resize(span, 0.0F); };
    auto        sum_into = [&](containers::Array<crd::f32>& dst, const containers::Array<crd::f32>& srcb) {
        for (usize k = 0; k < span; ++k) { dst[k] += srcb[k]; }
    };

    containers::Array<crd::f32> b0(&root); // %0 source@a
    containers::Array<crd::f32> b1(&root); // %1 gain
    containers::Array<crd::f32> b2(&root); // %2 biquad
    containers::Array<crd::f32> b3(&root); // %3 delay
    containers::Array<crd::f32> b4(&root); // %4 compressor
    containers::Array<crd::f32> b5(&root); // %5 source@b
    containers::Array<crd::f32> expect(&root);
    zeroed(b0);
    audio::apply_source(b0.data(), 0, n, cbinds[0].binding, 0, false);
    zeroed(b1);
    sum_into(b1, b0);
    audio::apply_gain(b1.data(), n, a_gain_db);
    zeroed(b2);
    sum_into(b2, b1);
    {
        crd::f64 z1[2] = {};
        crd::f64 z2[2] = {};
        audio::apply_biquad(b2.data(), n, a_filter, a_cutoff, a_q, z1, z2);
    }
    zeroed(b3);
    sum_into(b3, b2);
    audio::apply_delay(b3.data(), n, a_delay);
    zeroed(b4);
    sum_into(b4, b3);
    {
        crd::f64 env = 0.0;
        audio::apply_compressor(b4.data(), n, a_thr, a_ratio, a_attack, a_release, a_sr, env);
    }
    zeroed(b5);
    audio::apply_source(b5.data(), 0, n, cbinds[1].binding, 0, false);
    zeroed(expect);
    sum_into(expect, b4); // mix(%4, %5): sum both operands into a zeroed bus (operand order -- 2-input, commutative no-op)
    sum_into(expect, b5);

    REQUIRE(out.size() == expect.size());
    CHECK(std::memcmp(out.data(), expect.data(), out.size() * sizeof(crd::f32)) == 0);
}
