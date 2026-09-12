// CEIR-31a-1a — the ceir.audio dialect (sec-142; route the GEO-10 offline audio graph through CEIR, proof = BIT-EXACT vs
// render_graph at 31a-1b): the FIVE structural nodes (audio.source / gain / biquad / mix / send) over the CEIR-3d Tensor
// value + the semantic find_audio_misuse. Device-free (crd-ceir). Proves: (1) a well-formed source->gain->biquad + a
// 2-input mix module verifies clean AND survives a TEXT round-trip (print->parse->re-walk None + print==reprint), and the
// biquad filter vocab ADMITS all four {Lowpass,Highpass,Bandpass,Notch} (a 4-way identity so the filter check can't pass by
// matching 0 alone); (2) each semantic misuse is REJECTED with the EXACT AudioMisuseKind AND the offending OP identity
// (mirroring the shared AGRF graph_validate): OperandNotTensor, ResultTypeMismatch (an audio op preserves the buffer shape),
// BiquadFilterInvalid, BiquadCutoffInvalid, BiquadQInvalid, SourceStartFrameInvalid, and a mix same-shape ResultTypeMismatch.
// ⛔ 31a-1a-ii commits the assets/ceir/*.ceir fixture (bootstrap-via-print) + its anti-drift reading gate; 31a-1b lowers to
// host kernels + the bit-exact-vs-render_graph proof. ⛔ TEST NAMES STAY ASCII (a non-ASCII char mangles the ctest filter).

#include <crd/ceir/audio.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp> // register_resource_ops (resource.declare — the neutral typed-value seed)
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream> // slurp the committed audio .ceir asset (inline, mirroring test_dist_gate.cpp -- device-free tests/ceir
                   // never reaches into the tests/gpu-shared reader, a real layer boundary)

// ⛔ NO `#ifndef CRD_REPO_DIR / #define "."` fallback -- the CMakeLists (crd-ceir-tests PRIVATE CRD_REPO_DIR) ALWAYS defines
// it; a "." fallback is the cwd-luck scar pre-armed (Win-greens on ./assets, WSL-reds). A missing define must fail LOUD.

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
void register_audio_all(Context& ctx)
{
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)audio::register_audio_ops(ctx);
}

// slurp + parse a committed .ceir asset -- the cwd-luck REQUIRE(f.good()) in ONE place (a missing CRD_REPO_DIR asset must
// fail LOUD; on WSL CRD_REPO_DIR is the shared /mnt/d tree). `bytes` is CALLER-owned so it outlives the parse. (device-free
// tests/ceir slurps inline -- NO reach into the tests/gpu-shared reader, a real layer boundary.)
ParseResult load_committed_ceir(Context& ctx, const char* path, containers::Array<char>& bytes)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    bytes.resize(static_cast<usize>(sz), '\0');
    f.read(bytes.data(), sz);
    return parse(ctx, StringView(bytes.data(), bytes.size()));
}

// a resource.declare producing a fresh value of type `t` — a NEUTRAL (non-audio) seed find_audio_misuse ignores (the
// dist-gate mkval mold; an audio.source can't seed a non-tensor / would itself be the flagged op).
Value* mkval(Context& ctx, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t);
    b->append(d);
    return d->result(0U);
}

Block* mkmain(Context& ctx, Module& m)
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
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
// the audio value: a stereo f32 buffer as a Tensor<f32,[frames,2]>. ⛔ 31z (5): dim0 (frames) is DYNAMIC — the runtime
// block size — NOT a static extent (a baked static 256 was a latent contradiction: the executor's `frames` call param IS
// the block size, and the 1000-frame block-invariance test runs the SAME asset at a non-256 length). dim1 (channels) is
// Static 2 (stereo). The executor validates `frames` against dim0 only when dim0 is Static (a pinned block); Dynamic = any.
TypeId tensor_audio(Context& ctx)
{
    const TypeId d[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(2U)};
    return ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<TypeId>(d, 2U)));
}

// a source node producing a fresh audio value of type `t`.
Operation* mk_source(Context& ctx, Block* b, const char* name, TypeId t)
{
    Operation* const s = audio::build_source(ctx, ctx.attr_symbol(StringView(name)), ctx.attr_int(0), ctx.attr_int(0), t);
    b->append(s);
    return s;
}

// The ORACLE graph: main { %0=source@a; %1=gain(%0){-6dB}; %2=biquad(%1){Lowpass,0.25,0.7071}; %3=source@b;
// %4=mix(%2,%3) }. A DAG with a 2-input mix (so the operand/edge order is testable at 31a-1b).
Module* build_audio_module(Context& ctx)
{
    Module* const m  = ctx.create_module();
    Block* const  b  = mkmain(ctx, *m);
    const TypeId  tt = tensor_audio(ctx);
    Operation* const src1 = mk_source(ctx, b, "a", tt);
    Operation* const gn   = audio::build_gain(ctx, src1->result(0U), ctx.attr_float(-6.0), tt);
    b->append(gn);
    Operation* const bq =
        audio::build_biquad(ctx, gn->result(0U), ctx.attr_int(0), ctx.attr_float(0.25), ctx.attr_float(0.7071), tt);
    b->append(bq);
    Operation* const src2   = mk_source(ctx, b, "b", tt);
    Value* const     mix_in[2] = {bq->result(0U), src2->result(0U)};
    Operation* const mix    = ctx.create_operation(audio::mix_kind(ctx), ConstSpan<Value*>(mix_in, 2U), 1U, tt);
    b->append(mix);
    return m;
}

// The 31a-3 FULL-CHAIN oracle: main{sample_rate=48000} { %0=source@a; %1=gain{-6}; %2=biquad{Lowpass,0.25,0.7071};
// %3=delay{64}; %4=compressor{-20,4,5,20}; %5=source@b; %6=mix(%4,%5) } -- exercises ALL SIX node types + the sample_rate
// func attr in ONE committed asset (the complete sec-142 chain source->gain->biquad->delay->compressor->mix). The func
// carries sample_rate (the compressor needs it). ⛔ mix is 2-input so its operand ORDER is a commutative no-op (the scar);
// NOT an order-sensitivity gate (that needs a 3-input fixture -- 31z item 1).
Module* build_audio_full_chain_module(Context& ctx)
{
    Module* const m   = ctx.create_module();
    Block*        top = m->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m->body()->append(top);
    }
    Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    top->append(fn);
    ctx.set_attr(fn, "sample_rate", ctx.attr_int(48000)); // the graph-global rate the compressor's ms->coeff needs
    Block* const     b   = func::func_body_block(fn);
    const TypeId     tt  = tensor_audio(ctx);
    Operation* const s1  = mk_source(ctx, b, "a", tt);
    Operation* const gn  = audio::build_gain(ctx, s1->result(0U), ctx.attr_float(-6.0), tt);
    b->append(gn);
    Operation* const bq =
        audio::build_biquad(ctx, gn->result(0U), ctx.attr_int(0), ctx.attr_float(0.25), ctx.attr_float(0.7071), tt);
    b->append(bq);
    Operation* const dly = audio::build_delay(ctx, bq->result(0U), ctx.attr_int(64), tt);
    b->append(dly);
    Operation* const cmp = audio::build_compressor(ctx, dly->result(0U), ctx.attr_float(-20.0), ctx.attr_float(4.0),
                                                   ctx.attr_float(5.0), ctx.attr_float(20.0), tt);
    b->append(cmp);
    Operation* const s2        = mk_source(ctx, b, "b", tt);
    Value* const     mix_in[2] = {cmp->result(0U), s2->result(0U)};
    Operation* const mix       = ctx.create_operation(audio::mix_kind(ctx), ConstSpan<Value*>(mix_in, 2U), 1U, tt);
    b->append(mix);
    return m;
}

// The 31z ruling-(1) ORDER-SENSITIVITY oracle: main { %0=source@a; %1=source@b; %2=source@c; %3=mix(%0,%1,%2) } -- a
// 3-input mix, the MINIMAL topology whose operand ORDER is an OBSERVABLE f32 non-associative sum ((a+b)+c != a+(b+c)). The
// data is bound at execution (CeirSourceBinding), so the asset is pure TOPOLOGY; the execution order-sensitivity gate (CEIR
// == render_graph in the authored order, and a PERMUTED order differs) lives in test_ceir_audio_render.cpp.
Module* build_audio_mix3_module(Context& ctx)
{
    Module* const    m     = ctx.create_module();
    Block* const     b     = mkmain(ctx, *m);
    const TypeId     tt    = tensor_audio(ctx);
    Operation* const s0    = mk_source(ctx, b, "a", tt);
    Operation* const s1    = mk_source(ctx, b, "b", tt);
    Operation* const s2    = mk_source(ctx, b, "c", tt);
    Value* const     in[3] = {s0->result(0U), s1->result(0U), s2->result(0U)};
    Operation* const mix   = ctx.create_operation(audio::mix_kind(ctx), ConstSpan<Value*>(in, 3U), 1U, tt);
    b->append(mix);
    return m;
}
} // namespace

TEST_CASE("ceir 31a-1a: a well-formed audio module verifies and survives a text round-trip", "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);
    Module* const m = build_audio_module(ctx);

    CHECK(audio::find_audio_misuse(ctx, *m).kind == audio::AudioMisuseKind::None);

    // TEXT round-trip: the audio ops PRINT + re-PARSE to the identical canonical text and re-walk clean.
    const containers::String t0 = print(ctx, *m, &root);
    Context                  ctx2(&root);
    register_audio_all(ctx2);
    const ParseResult pr = parse(ctx2, StringView(t0.c_str(), t0.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    CHECK(audio::find_audio_misuse(ctx2, *pr.module).kind == audio::AudioMisuseKind::None);
    const containers::String t1 = print(ctx2, *pr.module, &root);
    CHECK(StringView(t1.c_str(), t1.size()) == StringView(t0.c_str(), t0.size()));

    // the biquad filter vocab ADMITS all FOUR {Lowpass,Highpass,Bandpass,Notch} — a 4-way identity so the filter check
    // can't pass the suite by matching 0 alone (the BiquadFilterInvalid REJECTION is the misuse TEST_CASE below).
    for (i64 filt = 0; filt <= 3; ++filt)
    {
        Module* const    mm = ctx.create_module();
        Block* const     bb = mkmain(ctx, *mm);
        const TypeId     tt = tensor_audio(ctx);
        Operation* const s  = mk_source(ctx, bb, "s", tt);
        bb->append(audio::build_biquad(ctx, s->result(0U), ctx.attr_int(filt), ctx.attr_float(0.25),
                                       ctx.attr_float(0.7071), tt));
        CHECK(audio::find_audio_misuse(ctx, *mm).kind == audio::AudioMisuseKind::None);
    }
}

TEST_CASE("ceir 31a-1a: each audio misuse is rejected with the exact kind and offending op", "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);
    Module* const m  = ctx.create_module();
    Block* const  b  = mkmain(ctx, *m);
    const TypeId  tt = tensor_audio(ctx);

    SECTION("operand-not-tensor: a gain on a non-tensor value")
    {
        Value* const     in = mkval(ctx, b, ctx.type_i32()); // a neutral i32 seed (resource.declare — not an audio op)
        Operation* const gn = audio::build_gain(ctx, in, ctx.attr_float(0.0), tt);
        b->append(gn);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::OperandNotTensor);
        CHECK(e.op == gn);
        CHECK(e.value == in);
    }
    SECTION("result-type-mismatch: a gain whose result type differs from its input")
    {
        Operation* const src = mk_source(ctx, b, "a", tt);
        const TypeId     bad = ctx.type_tensor(ctx.type_f32(), sh2(ctx, 128U, 2U)); // != input [256,2]
        Operation* const gn  = audio::build_gain(ctx, src->result(0U), ctx.attr_float(0.0), bad);
        b->append(gn);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::ResultTypeMismatch);
        CHECK(e.op == gn);
    }
    SECTION("biquad-filter-invalid: a filter beyond Notch")
    {
        Operation* const src = mk_source(ctx, b, "a", tt);
        Operation* const bq =
            audio::build_biquad(ctx, src->result(0U), ctx.attr_int(4), ctx.attr_float(0.25), ctx.attr_float(0.7071), tt);
        b->append(bq);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::BiquadFilterInvalid);
        CHECK(e.op == bq);
    }
    SECTION("biquad-filter-invalid: a negative filter index (the LOWER bound, so fl.i < 0 is not dead)")
    {
        Operation* const src = mk_source(ctx, b, "a", tt);
        Operation* const bq =
            audio::build_biquad(ctx, src->result(0U), ctx.attr_int(-1), ctx.attr_float(0.25), ctx.attr_float(0.7071), tt);
        b->append(bq);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::BiquadFilterInvalid);
        CHECK(e.op == bq);
    }
    SECTION("biquad-cutoff-invalid: a cutoff outside (0,1)")
    {
        Operation* const src = mk_source(ctx, b, "a", tt);
        Operation* const bq =
            audio::build_biquad(ctx, src->result(0U), ctx.attr_int(0), ctx.attr_float(1.5), ctx.attr_float(0.7071), tt);
        b->append(bq);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::BiquadCutoffInvalid);
        CHECK(e.op == bq);
    }
    SECTION("biquad-q-invalid: a non-positive Q")
    {
        Operation* const src = mk_source(ctx, b, "a", tt);
        Operation* const bq =
            audio::build_biquad(ctx, src->result(0U), ctx.attr_int(0), ctx.attr_float(0.25), ctx.attr_float(0.0), tt);
        b->append(bq);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::BiquadQInvalid);
        CHECK(e.op == bq);
    }
    SECTION("source-start-frame-invalid: a negative start frame")
    {
        Operation* const s = audio::build_source(ctx, ctx.attr_symbol(StringView("a")), ctx.attr_int(-1),
                                                 ctx.attr_int(0), tt);
        b->append(s);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::SourceStartFrameInvalid);
        CHECK(e.op == s);
    }
    SECTION("mix result-type-mismatch: a variadic input whose shape differs")
    {
        const TypeId     bad = ctx.type_tensor(ctx.type_f32(), sh2(ctx, 128U, 2U)); // != [256,2]
        Value* const     v1  = mkval(ctx, b, tt);
        Value* const     v2  = mkval(ctx, b, bad); // a second input of a DIFFERENT shape
        Value* const     mix_in[2] = {v1, v2};
        Operation* const mix       = ctx.create_operation(audio::mix_kind(ctx), ConstSpan<Value*>(mix_in, 2U), 1U, tt);
        b->append(mix);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::ResultTypeMismatch);
        CHECK(e.op == mix);
    }
    // ⛔ CEIR-31z ruling (5) — the SHAPE is a declared header word, validated at cook time (the declared-words family).
    SECTION("tensor-rank-invalid: an audio value that is not rank-2 [frames, 2]")
    {
        const TypeId     r1[1] = {ctx.type_dim_static(256U)}; // a rank-1 shape
        const TypeId     bad   = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<TypeId>(r1, 1U)));
        Operation* const s = audio::build_source(ctx, ctx.attr_symbol(StringView("a")), ctx.attr_int(0), ctx.attr_int(0), bad);
        b->append(s);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::TensorRankInvalid);
        CHECK(e.op == s);
    }
    SECTION("channel-count-invalid: a channel dim (dim1) that is not Static 2 (stereo)")
    {
        const TypeId     bad = ctx.type_tensor(ctx.type_f32(), sh2(ctx, 256U, 3U)); // [256, 3] -- 3 channels
        Operation* const s = audio::build_source(ctx, ctx.attr_symbol(StringView("a")), ctx.attr_int(0), ctx.attr_int(0), bad);
        b->append(s);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::ChannelCountInvalid);
        CHECK(e.op == s);
    }
    SECTION("frame-dim-invalid: a Symbolic frame dim (dim0) -- not a Static>=1 or Dynamic block size")
    {
        const TypeId     sym[2] = {ctx.type_dim_symbolic(StringView("N")), ctx.type_dim_static(2U)};
        const TypeId     bad    = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<TypeId>(sym, 2U)));
        Operation* const s = audio::build_source(ctx, ctx.attr_symbol(StringView("a")), ctx.attr_int(0), ctx.attr_int(0), bad);
        b->append(s);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::FrameDimInvalid);
        CHECK(e.op == s);
    }
    SECTION("valid: BOTH a Dynamic frame dim [dyn,2] and a pinned Static frame dim [256,2] verify clean")
    {
        Operation* const sd = mk_source(ctx, b, "a", tt); // [dyn, 2] -- the runtime block size
        b->append(sd);
        const TypeId     stat = ctx.type_tensor(ctx.type_f32(), sh2(ctx, 256U, 2U)); // [256, 2] -- a pinned block
        Operation* const ss = audio::build_source(ctx, ctx.attr_symbol(StringView("b")), ctx.attr_int(0), ctx.attr_int(0), stat);
        b->append(ss);
        CHECK(audio::find_audio_misuse(ctx, *m).kind == audio::AudioMisuseKind::None);
    }
}

TEST_CASE("ceir 31a-1a-ii: the committed audio_source_gain_biquad_mix.ceir is anti-drift and roundtrip-stable",
          "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);

    // the in-memory ORACLE + its canonical print (the anti-drift SOURCE -- regenerate the asset if build_audio_module changes;
    // the asset was BOOTSTRAPPED by printing this oracle to the file [binary mode, LF-only] then the write stripped).
    Module* const            built   = build_audio_module(ctx);
    const containers::String t_built = print(ctx, *built, &root);

    // parse-load the COMMITTED asset.
    containers::Array<char> src(ctx.allocator());
    const ParseResult       pr =
        load_committed_ceir(ctx, CRD_REPO_DIR "/assets/ceir/audio_source_gain_biquad_mix.ceir", src);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ANTI-DRIFT: the committed file's canonical print == the oracle's (never file-bytes -- regenerate on a builder change).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP stability: print(parse(file)) re-parses to the identical canonical text (the committed file IS canonical).
    Context ctx2(&root);
    register_audio_all(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // the loaded asset is well-formed BOTH ways: SEMANTICALLY (find_audio_misuse) AND STRUCTURALLY (find_structure_error --
    // the DAG / StateEdge-feedback rule; the func body is a Graph region so no terminator is required, the audio graph a
    // pure DAG so no back-edge -- closing the 31a-1a-i gate's structure-verifier gap the advisor flagged).
    CHECK(audio::find_audio_misuse(ctx, *pr.module).kind == audio::AudioMisuseKind::None);
    CHECK(ctx.find_structure_error(*pr.module).kind == StructureErrorKind::None);
}

TEST_CASE("ceir 31z-(1): the committed audio_mix3.ceir (a 3-input mix) is anti-drift, roundtrip-stable, and complete",
          "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);

    // the in-memory ORACLE + its canonical print (the anti-drift SOURCE -- regen the asset if build_audio_mix3_module changes;
    // bootstrapped via print [binary, LF-only], the 31a-1a-ii mold, [[reference_ceir_text_asset_authoring_via_print]]).
    Module* const            built   = build_audio_mix3_module(ctx);
    const containers::String t_built = print(ctx, *built, &root);

    containers::Array<char> src(ctx.allocator());
    const ParseResult       pr = load_committed_ceir(ctx, CRD_REPO_DIR "/assets/ceir/audio_mix3.ceir", src);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ANTI-DRIFT through the printer (never file-bytes): the committed file's canonical print == the oracle's.
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP-stable + well-formed BOTH ways (a pure DAG -- the 3 sources fan into one mix, no back-edge).
    Context ctx2(&root);
    register_audio_all(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));
    CHECK(audio::find_audio_misuse(ctx, *pr.module).kind == audio::AudioMisuseKind::None);
    CHECK(ctx.find_structure_error(*pr.module).kind == StructureErrorKind::None);

    // ⛔ COMPLETENESS (the mold's blind spot -- a node-DELETED asset also parses+verifies clean): EXACTLY 4 ops = 3
    // audio.source + 1 audio.mix, and the mix sums EXACTLY 3 operands (a 2-input mix parses clean but is a commutative
    // no-op -- the order-sensitivity NEEDS 3, the 31a-3 mix's 2-input blind spot the tracker flagged).
    Operation* pf = nullptr;
    for (Operation* op = pr.module->body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == StringView("func.func")) { pf = op; break; }
    }
    REQUIRE(pf != nullptr);
    Block* const fb = pf->region(0)->first_block();
    REQUIRE(fb != nullptr);
    u32              n_ops   = 0;
    u32              n_source = 0;
    u32              n_mix    = 0;
    const Operation* mix_op  = nullptr;
    for (Operation* op = fb->first_op(); op != nullptr; op = op->next_in_block())
    {
        const StringView nm = ctx.op_name(op->kind());
        ++n_ops;
        if (nm == StringView("audio.source")) { ++n_source; }
        else if (nm == StringView("audio.mix"))
        {
            ++n_mix;
            mix_op = op;
        }
    }
    CHECK(n_ops == 4U);
    CHECK(n_source == 3U);
    CHECK(n_mix == 1U);
    REQUIRE(mix_op != nullptr);
    CHECK(mix_op->num_operands() == 3U); // the 3-input order-sensitivity topology
}

TEST_CASE("ceir 31z-(2): the delay StateEdge exemption -- a delay-headed feedback echo is structurally LEGAL, a "
          "combinational (Pure-headed) cycle is FeedbackWithoutState",
          "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);
    const TypeId tt = tensor_audio(ctx);

    SECTION("delay-headed feedback echo delay(mix(src, delay)) is LEGAL -- the StateEdge back-edge exemption EXERCISED")
    {
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Operation* const src = mk_source(ctx, b, "a", tt);
        Operation* const dly = audio::build_delay(ctx, src->result(0U), ctx.attr_int(64), tt); // temp operand = src
        b->append(dly);
        Value* const     mix_in[2] = {src->result(0U), dly->result(0U)};
        Operation* const mix       = ctx.create_operation(audio::mix_kind(ctx), ConstSpan<Value*>(mix_in, 2U), 1U, tt);
        b->append(mix);
        dly->set_operand(0U, mix->result(0U)); // ⛔ the BACK-EDGE: delay's (only=last) operand = mix's result (defined LATER).
        // The delay HEADS the cycle and carries OpTrait::StateEdge, so the 5d verifier accepts the back-edge (NOT
        // FeedbackWithoutState) -- the echo exemption the straight-line §142 chain DECLARED but never EXERCISED.
        CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
        CHECK(audio::find_audio_misuse(ctx, *m).kind == audio::AudioMisuseKind::None);
    }
    SECTION("the SAME cycle through a Pure gain gain(mix(src, gain)) is FeedbackWithoutState -- the TRAIT admits it, not the name")
    {
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Operation* const src = mk_source(ctx, b, "a", tt);
        Operation* const gn  = audio::build_gain(ctx, src->result(0U), ctx.attr_float(0.0), tt); // temp operand = src
        b->append(gn);
        Value* const     mix_in[2] = {src->result(0U), gn->result(0U)};
        Operation* const mix       = ctx.create_operation(audio::mix_kind(ctx), ConstSpan<Value*>(mix_in, 2U), 1U, tt);
        b->append(mix);
        gn->set_operand(0U, mix->result(0U)); // gn reads mix (defined LATER); gain is Pure (no StateEdge) => combinational
        const auto e = ctx.find_structure_error(*m);
        CHECK(e.kind == StructureErrorKind::FeedbackWithoutState);
        CHECK(e.op == gn); // the pointing contract: the offender is the Pure op carrying the back-edge (the test_daw mold)
    }
}

TEST_CASE("ceir 31a-2a: audio.delay verifies clean and rejects a negative delay by identity", "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);
    const TypeId tt = tensor_audio(ctx);

    // WELL-FORMED: source -> delay{D} for D = 0 (identity) and D = 5 (>= 0, NO upper bound).
    for (i64 d = 0; d <= 5; d += 5)
    {
        Module* const    m = ctx.create_module();
        Block* const     b = mkmain(ctx, *m);
        Operation* const s = mk_source(ctx, b, "s", tt);
        b->append(audio::build_delay(ctx, s->result(0U), ctx.attr_int(d), tt));
        CHECK(audio::find_audio_misuse(ctx, *m).kind == audio::AudioMisuseKind::None);
    }

    // MISUSE: a negative delay -> DelayFramesInvalid, pointing at the delay op.
    {
        Module* const    m   = ctx.create_module();
        Block* const     b   = mkmain(ctx, *m);
        Operation* const s   = mk_source(ctx, b, "s", tt);
        Operation* const dly = audio::build_delay(ctx, s->result(0U), ctx.attr_int(-1), tt);
        b->append(dly);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::DelayFramesInvalid);
        CHECK(e.op == dly);
    }
}

TEST_CASE("ceir 31a-2b: audio.compressor verifies clean and rejects each bad param by identity", "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);
    const TypeId tt = tensor_audio(ctx);

    // a source -> compressor{threshold_db, ratio, attack_ms, release_ms} of `tt`.
    auto mk_comp = [&](Module& m, f64 thr, f64 ratio, f64 atk, f64 rel) -> Operation* {
        Block* const     b = mkmain(ctx, m);
        Operation* const s = mk_source(ctx, b, "s", tt);
        Operation* const c = audio::build_compressor(ctx, s->result(0U), ctx.attr_float(thr), ctx.attr_float(ratio),
                                                     ctx.attr_float(atk), ctx.attr_float(rel), tt);
        b->append(c);
        return c;
    };

    // WELL-FORMED: a typical compressor AND the ratio == 1 LOWER boundary (>= 1 is legal, so `ratio >= 1` is not dead at the
    // edge) AND a NEGATIVE threshold_db (unrestricted -- any dB, so no threshold check exists to spuriously reject).
    {
        Module* const m = ctx.create_module();
        (void)mk_comp(*m, -20.0, 4.0, 10.0, 100.0);
        CHECK(audio::find_audio_misuse(ctx, *m).kind == audio::AudioMisuseKind::None);
    }
    {
        Module* const m = ctx.create_module();
        (void)mk_comp(*m, 6.0, 1.0, 0.5, 250.0); // ratio == 1 (boundary), positive threshold
        CHECK(audio::find_audio_misuse(ctx, *m).kind == audio::AudioMisuseKind::None);
    }

    // MISUSE (each keeps the OTHER params valid so the SPECIFIC kind fires, in check_op order ratio->attack->release):
    SECTION("compressor-ratio-invalid: a ratio below 1")
    {
        Module* const          m = ctx.create_module();
        Operation* const       c = mk_comp(*m, -20.0, 0.5, 10.0, 100.0);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::CompressorRatioInvalid);
        CHECK(e.op == c);
    }
    SECTION("compressor-attack-invalid: a non-positive attack time")
    {
        Module* const          m = ctx.create_module();
        Operation* const       c = mk_comp(*m, -20.0, 4.0, 0.0, 100.0);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::CompressorAttackInvalid);
        CHECK(e.op == c);
    }
    SECTION("compressor-release-invalid: a negative release time")
    {
        Module* const          m = ctx.create_module();
        Operation* const       c = mk_comp(*m, -20.0, 4.0, 10.0, -1.0);
        const audio::AudioMisuse e = audio::find_audio_misuse(ctx, *m);
        CHECK(e.kind == audio::AudioMisuseKind::CompressorReleaseInvalid);
        CHECK(e.op == c);
    }
}

TEST_CASE("ceir 31a-2b-ii: the sample_rate func attr round-trips through text and SampleRateInvalid rejects a bad rate",
          "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);
    const TypeId tt = tensor_audio(ctx);

    // the graph-global sample_rate rides func.func as an int attr (NOT a body op; NOT the exec call) -- authorable in .ceir
    // text, the AudioGraphResource.sample_rate + recursion-policy func-attr precedent. Build main{sample_rate} with a
    // source -> compressor body and the given rate on the func op.
    struct Built
    {
        Module*    m;
        Operation* fn;
    };
    auto build = [&](AttrId sr) -> Built { // sr passed as an AttrId so a section can author a NON-Int slip (attr_float)
        Module* const m   = ctx.create_module();
        Block*        top = m->body()->first_block();
        if (top == nullptr)
        {
            top = ctx.create_block(0U);
            m->body()->append(top);
        }
        Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
        top->append(fn);
        ctx.set_attr(fn, "sample_rate", sr);
        Block* const     b = func::func_body_block(fn);
        Operation* const s = mk_source(ctx, b, "s", tt);
        b->append(audio::build_compressor(ctx, s->result(0U), ctx.attr_float(-20.0), ctx.attr_float(4.0),
                                          ctx.attr_float(10.0), ctx.attr_float(100.0), tt));
        return {m, fn};
    };

    SECTION("well-formed 48000 verifies AND the sample_rate attr survives print -> parse -> print")
    {
        const Built built = build(ctx.attr_int(48000));
        CHECK(audio::find_audio_misuse(ctx, *built.m).kind == audio::AudioMisuseKind::None);

        // ⛔ the WHOLE point of "rides the graph" is TEXT authorability -- prove the printer EMITS the func attr and the
        // parser RE-READS it (a printer-only or reserved-vocab attr would be authorable-in-C++ but not in .ceir text).
        const containers::String t0 = print(ctx, *built.m, &root);
        Context                  ctx2(&root);
        register_audio_all(ctx2);
        const ParseResult pr = parse(ctx2, StringView(t0.c_str(), t0.size()));
        REQUIRE(pr.ok);
        REQUIRE(pr.module != nullptr);
        const containers::String t1 = print(ctx2, *pr.module, &root);
        CHECK(StringView(t1.c_str(), t1.size()) == StringView(t0.c_str(), t0.size()));

        Operation* pf = nullptr;
        for (Operation* op = pr.module->body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx2.op_name(op->kind()) == StringView("func.func")) { pf = op; break; }
        }
        REQUIRE(pf != nullptr);
        const AttrId sra = pf->attr("sample_rate");
        REQUIRE(sra.valid()); // the attr SURVIVED the round-trip (else it is not text-authorable)
        CHECK(ctx2.attr_value(sra).kind == AttrKind::Int);
        CHECK(ctx2.attr_value(sra).i == 48000);
        CHECK(audio::find_audio_misuse(ctx2, *pr.module).kind == audio::AudioMisuseKind::None);
    }

    SECTION("sample-rate-invalid: a zero rate is rejected pointing at the func op")
    {
        const Built              built = build(ctx.attr_int(0));
        const audio::AudioMisuse e     = audio::find_audio_misuse(ctx, *built.m);
        CHECK(e.kind == audio::AudioMisuseKind::SampleRateInvalid);
        CHECK(e.op == built.fn);
    }
    SECTION("sample-rate-invalid: a NON-Int rate (48000.0 Float -- an authoring slip) is rejected; no generated verifier "
            "owns func.func's attr, so find_audio_misuse is the only guard before the executor's silent reject")
    {
        const Built              built = build(ctx.attr_float(48000.0)); // a Float, not Int -- the reachable kind arm
        const audio::AudioMisuse e     = audio::find_audio_misuse(ctx, *built.m);
        CHECK(e.kind == audio::AudioMisuseKind::SampleRateInvalid);
        CHECK(e.op == built.fn);
    }
}

TEST_CASE("ceir 31a-3: the committed audio_full_chain.ceir (all six nodes + sample_rate) is anti-drift, complete, roundtrip",
          "[ceir][audio]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_audio_all(ctx);

    // the in-memory ORACLE + its canonical print (the anti-drift SOURCE -- regen the asset if build_audio_full_chain_module
    // changes; the asset was BOOTSTRAPPED by printing this oracle [binary, LF-only] then the write stripped -- the 31a-1a-ii
    // mold, [[reference_ceir_text_asset_authoring_via_print]]).
    Module* const            built   = build_audio_full_chain_module(ctx);
    const containers::String t_built = print(ctx, *built, &root);

    // parse-load the COMMITTED asset (bootstrapped via print [binary, LF-only] then the write STRIPPED -- the 31a-1a-ii mold).
    containers::Array<char> src(ctx.allocator());
    const ParseResult       pr = load_committed_ceir(ctx, CRD_REPO_DIR "/assets/ceir/audio_full_chain.ceir", src);
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ANTI-DRIFT through the printer (never file-bytes): the committed file's canonical print == the oracle's.
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP-stable + well-formed BOTH ways (SEMANTIC find_audio_misuse + STRUCTURAL find_structure_error -- a pure DAG,
    // Graph region so no terminator; the delay/compressor StateEdge nodes are in the FORWARD chain, so no back-edge here).
    Context ctx2(&root);
    register_audio_all(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));
    CHECK(audio::find_audio_misuse(ctx, *pr.module).kind == audio::AudioMisuseKind::None);
    CHECK(ctx.find_structure_error(*pr.module).kind == StructureErrorKind::None);

    // the func op from the COMMITTED FILE (not the in-memory print -- #650 proved in-memory): sample_rate survives to DISK.
    Operation* pf = nullptr;
    for (Operation* op = pr.module->body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == StringView("func.func")) { pf = op; break; }
    }
    REQUIRE(pf != nullptr);
    const AttrId sra = pf->attr("sample_rate");
    REQUIRE(sra.valid());
    CHECK(ctx.attr_value(sra).kind == AttrKind::Int);
    CHECK(ctx.attr_value(sra).i == 48000);

    // COMPLETENESS (the mold's blind spot -- a node-DELETED asset also parses clean + verifies None): the func body has
    // EXACTLY 7 ops and every one of the six node types appears (audio.source x2). Anti "someone deleted the delay line
    // from the asset and the reading gate stayed green" -- the reading arms above can't see a missing node.
    Block* const fb = pf->region(0)->first_block();
    REQUIRE(fb != nullptr);
    u32 n_ops        = 0;
    u32 n_source     = 0;
    u32 n_gain       = 0;
    u32 n_biquad     = 0;
    u32 n_delay      = 0;
    u32 n_compressor = 0;
    u32 n_mix        = 0;
    for (Operation* op = fb->first_op(); op != nullptr; op = op->next_in_block())
    {
        const StringView nm = ctx.op_name(op->kind());
        ++n_ops;
        if (nm == StringView("audio.source")) { ++n_source; }
        else if (nm == StringView("audio.gain")) { ++n_gain; }
        else if (nm == StringView("audio.biquad")) { ++n_biquad; }
        else if (nm == StringView("audio.delay")) { ++n_delay; }
        else if (nm == StringView("audio.compressor")) { ++n_compressor; }
        else if (nm == StringView("audio.mix")) { ++n_mix; }
    }
    CHECK(n_ops == 7U);
    CHECK(n_source == 2U);
    CHECK(n_gain == 1U);
    CHECK(n_biquad == 1U);
    CHECK(n_delay == 1U);
    CHECK(n_compressor == 1U);
    CHECK(n_mix == 1U);
}
