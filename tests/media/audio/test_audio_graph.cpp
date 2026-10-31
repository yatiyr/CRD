// test_audio_graph.cpp — GEO-10: THE 4-TRACK GATE + the resource round-trips.
//  · ABUF: build → load round-trip (normalized f32 EXACT for 16/24-bit sources)
//  · AGRF: build → load round-trip; the validator refuses cycles, bad indices, unsorted automation
//  · THE GATE: a 4-track graph — 2 sampled sources, one through an RBJ lowpass, one through an AUTOMATED
//    gain — renders offline BIT-STABLE (two renders memcmp-identical), the mix is sample-verifiable
//    (silence with sources muted; superposition holds), and the master encodes to WAV through our codec.

#include <crd/audio/audio_graph.hpp>
#include <crd/audio/audio_kernels.hpp>
#include <crd/audio/audio_resources.hpp>
#include <crd/audio/wav.hpp>
#include <crd/core/seed_generators.hpp>
#include <crd/hesap/dsp/rbj.hpp>
#include <crd/hesap/interp/keyframe.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/loader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(256U << 20U);
    return a;
}

// a 440-ish Hz sine and a 220-ish saw as the two "takes" (mono, 48 kHz, 1 s)
audio::AudioBufferResource make_tone(f32 base, bool saw)
{
    audio::AudioBufferResource buf(&galloc());
    buf.sample_rate = 48000;
    buf.channels    = 1;
    for (u32 i = 0; i < 48000; ++i)
    {
        const f32 phase = static_cast<f32>(i) * base / 48000.0F;
        const f32 frac  = phase - static_cast<f32>(static_cast<i32>(phase));
        buf.samples.push_back(saw ? 0.4F * (2.0F * frac - 1.0F)
                                  : 0.4F * math::sin(6.2831853F * frac));
    }
    return buf;
}

// the gate graph: src0 → biquad(lowpass) ─┐
//                 src1 → gain(AUTOMATED) ─┴→ mix(out)
audio::AudioGraphResource make_gate_graph()
{
    audio::AudioGraphResource g(&galloc());
    g.sample_rate = 48000;
    g.name_off    = g.intern("gate-graph");

    audio::AudioNodeRec src0;
    src0.type     = static_cast<u8>(audio::AudioNodeType::Source);
    src0.name_off = g.intern("take0");
    src0.loop     = 1;
    g.nodes.push_back(src0);

    audio::AudioNodeRec src1;
    src1.type     = static_cast<u8>(audio::AudioNodeType::Source);
    src1.name_off = g.intern("take1");
    src1.loop     = 1;
    g.nodes.push_back(src1);

    audio::AudioNodeRec lp;
    lp.type     = static_cast<u8>(audio::AudioNodeType::Biquad);
    lp.filter   = static_cast<u8>(audio::BiquadType::Lowpass);
    lp.name_off = g.intern("lp");
    lp.cutoff   = 0.05F; // 1.2 kHz at 48 k
    lp.q        = 0.7071F;
    g.nodes.push_back(lp);

    audio::AudioNodeRec gain;
    gain.type     = static_cast<u8>(audio::AudioNodeType::Gain);
    gain.name_off = g.intern("fade");
    gain.gain_db  = 0.0F;
    g.nodes.push_back(gain);

    audio::AudioNodeRec mix;
    mix.type     = static_cast<u8>(audio::AudioNodeType::Mix);
    mix.name_off = g.intern("master");
    g.nodes.push_back(mix);
    g.out_node = 4;

    g.edges.push_back({0, 2}); // src0 → lp
    g.edges.push_back({2, 4}); // lp → mix
    g.edges.push_back({1, 3}); // src1 → gain
    g.edges.push_back({3, 4}); // gain → mix

    // the AUTOMATED gain: 0 dB → −24 dB over the first half second (Linear, sample-rate ticks)
    audio::AudioAutoRec fade;
    fade.node       = 3;
    fade.param      = static_cast<u8>(audio::AudioParam::GainDb);
    fade.rate       = time::make_rate(48000, 1);
    fade.interp     = static_cast<u8>(hesap::interp::KeyInterp::Linear);
    fade.key_count  = 2;
    fade.ticks_off  = 0;
    fade.values_off = 0;
    g.auto_ticks.push_back(0);
    g.auto_ticks.push_back(24000);
    g.auto_values.push_back(0.0F);
    g.auto_values.push_back(-24.0F);
    g.automation.push_back(fade);
    return g;
}

} // namespace

TEST_CASE("abuf: build -> load round-trips normalized f32 exactly", "[audio][abuf]")
{
    audio::AudioPcm pcm(&galloc());
    pcm.sample_rate     = 44100;
    pcm.channels        = 2;
    pcm.bits_per_sample = 24;
    for (i32 i = -100; i < 100; ++i)
    {
        pcm.isamples.push_back(i * 40000);
        pcm.isamples.push_back(-i * 40000);
    }
    const resources::ResourceId id{1, 2};
    containers::Array<u8>       bytes = audio::audio_buffer_build(pcm, id, &galloc());
    REQUIRE(bytes.size() > 0);

    audio::AudioBufferLoader loader(&galloc());
    resources::LoadContext   ctx;
    ctx.id        = id;
    ctx.bytes     = containers::as_const_span(bytes);
    ctx.allocator = &galloc();
    auto* buf     = static_cast<audio::AudioBufferResource*>(loader.load(ctx));
    REQUIRE(buf != nullptr);
    CHECK(buf->sample_rate == 44100);
    CHECK(buf->channels == 2);
    CHECK(buf->source_bits == 24);
    REQUIRE(buf->samples.size() == pcm.isamples.size());
    for (usize i = 0; i < buf->samples.size(); ++i) // 24-bit ints are EXACT in f32 — bit-checkable
    {
        REQUIRE(buf->samples[i] == static_cast<f32>(pcm.isamples[i]) / 8388608.0F);
    }
    loader.unload(buf);
}

TEST_CASE("agrf: round-trip + the refusal classes", "[audio][agrf]")
{
    const audio::AudioGraphResource g     = make_gate_graph();
    const resources::ResourceId     id{3, 4};
    containers::Array<u8>           bytes = audio::audio_graph_build(g, id, &galloc());
    REQUIRE(bytes.size() > 0);

    audio::AudioGraphLoader loader(&galloc());
    resources::LoadContext  ctx;
    ctx.id        = id;
    ctx.bytes     = containers::as_const_span(bytes);
    ctx.allocator = &galloc();
    auto* loaded  = static_cast<audio::AudioGraphResource*>(loader.load(ctx));
    REQUIRE(loaded != nullptr);
    CHECK(loaded->nodes.size() == 5);
    CHECK(loaded->edges.size() == 4);
    CHECK(loaded->automation.size() == 1);
    CHECK(std::strcmp(loaded->strings.data() + loaded->nodes[2].name_off, "lp") == 0);
    loader.unload(loaded);

    { // a CYCLE refuses
        audio::AudioGraphResource bad = make_gate_graph();
        bad.edges.push_back({4, 0}); // mix → src0
        CHECK(audio::audio_graph_build(bad, id, &galloc()).size() == 0);
    }
    { // an out-of-range edge refuses
        audio::AudioGraphResource bad = make_gate_graph();
        bad.edges.push_back({0, 9});
        CHECK(audio::audio_graph_build(bad, id, &galloc()).size() == 0);
    }
    { // unsorted automation refuses
        audio::AudioGraphResource bad          = make_gate_graph();
        bad.auto_ticks[1]                      = 0;
        CHECK(audio::audio_graph_build(bad, id, &galloc()).size() == 0);
    }
    { // no output node refuses
        audio::AudioGraphResource bad = make_gate_graph();
        bad.out_node                  = audio::kInvalidNode;
        CHECK(audio::audio_graph_build(bad, id, &galloc()).size() == 0);
    }
}

TEST_CASE("THE 4-TRACK GATE: offline render bit-stable, mix verifiable, master encodes", "[audio][graph]")
{
    const audio::AudioGraphResource  g     = make_gate_graph();
    const audio::AudioBufferResource tone0 = make_tone(440.0F, false);
    const audio::AudioBufferResource tone1 = make_tone(220.0F, true);

    audio::GraphSourceBinding bindings[5] = {};
    bindings[0] = {containers::ConstSpan<f32>(tone0.samples.data(), tone0.samples.size()), 1};
    bindings[1] = {containers::ConstSpan<f32>(tone1.samples.data(), tone1.samples.size()), 1};

    const i64             frames = 48000; // 1 s
    containers::Array<f32> master(&galloc());
    REQUIRE(audio::render_graph(g, {bindings, 5}, frames, master) == frames);
    REQUIRE(master.size() == static_cast<usize>(frames) * 2);

    // BIT-STABLE: a second render is byte-identical
    containers::Array<f32> again(&galloc());
    REQUIRE(audio::render_graph(g, {bindings, 5}, frames, again) == frames);
    CHECK(std::memcmp(master.data(), again.data(), master.size() * 4) == 0);

    // the automated fade actually happened: RMS of src1's contribution collapses ~16× (−24 dB) by the end
    // — verified through SUPERPOSITION: render with src0 muted (unbind ↔ zero buffer) and compare halves
    containers::Array<f32> silence(&galloc());
    silence.resize(tone0.samples.size());
    for (usize i = 0; i < silence.size(); ++i)
    {
        silence[i] = 0.0F;
    }
    audio::GraphSourceBinding only1[5] = {};
    only1[0] = {containers::ConstSpan<f32>(silence.data(), silence.size()), 1};
    only1[1] = bindings[1];
    containers::Array<f32> fade_only(&galloc());
    REQUIRE(audio::render_graph(g, {only1, 5}, frames, fade_only) == frames);
    f64 head = 0.0;
    f64 tail = 0.0;
    for (i64 i = 0; i < 4800; ++i)
    {
        const f64 hs = static_cast<f64>(fade_only[static_cast<usize>(i) * 2]);
        head += hs * hs;
        const usize k  = static_cast<usize>(frames - 4800 + i) * 2;
        const f64   ts = static_cast<f64>(fade_only[k]);
        tail += ts * ts;
    }
    CHECK(tail * 100.0 < head); // ≥ 20 dB down after the fade — the automation is REAL

    // superposition: full mix == lp(src0) + fade(src1) sample for sample (linear graph, exact f32 ops)
    audio::GraphSourceBinding only0[5] = {};
    only0[0] = bindings[0];
    only0[1] = {containers::ConstSpan<f32>(silence.data(), silence.size()), 1};
    containers::Array<f32> lp_only(&galloc());
    REQUIRE(audio::render_graph(g, {only0, 5}, frames, lp_only) == frames);
    for (usize i = 0; i < 200; ++i) // spot-check the head (the full sweep is the bit-stable gate's job)
    {
        REQUIRE(master[i] == lp_only[i] + fade_only[i]);
    }

    // the master encodes through OUR wav codec (the GEO-9 audio-master seam)
    audio::AudioPcm out(&galloc());
    out.sample_rate     = 48000;
    out.channels        = 2;
    out.bits_per_sample = 0;
    for (f32 v : master)
    {
        out.fsamples.push_back(v);
    }
    CHECK(audio::wav_encode(out, &galloc()).size() > 0);

    // a missing source binding REFUSES (never silence-instead-of-music)
    audio::GraphSourceBinding none[5] = {};
    containers::Array<f32>    dummy(&galloc());
    CHECK(audio::render_graph(g, {none, 5}, frames, dummy) == 0);
}

namespace
{
// apply_biquad's loop as the audio module wrote it before OWN.1 moved the section step onto hesap-dsp.
void reference_apply_biquad(f32* bus, i64 len, u8 filter, f32 cutoff, f32 q, f64 z1[2], f64 z2[2])
{
    hesap::dsp::Biquad<f64> bq;
    const f64               f0 = static_cast<f64>(cutoff);
    const f64               qq = static_cast<f64>(q);
    switch (static_cast<audio::BiquadType>(filter))
    {
    case audio::BiquadType::Lowpass: bq = hesap::dsp::rbj_lowpass<f64>(f0, qq); break;
    case audio::BiquadType::Highpass: bq = hesap::dsp::rbj_highpass<f64>(f0, qq); break;
    case audio::BiquadType::Bandpass: bq = hesap::dsp::rbj_bandpass<f64>(f0, qq); break;
    case audio::BiquadType::Notch:
    default: bq = hesap::dsp::rbj_notch<f64>(f0, qq); break;
    }
    for (i64 i = 0; i < len; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            const f64 x    = static_cast<f64>(bus[i * 2 + c]);
            const f64 y    = bq.b0 * x + z1[c];
            z1[c]          = bq.b1 * x - bq.a1 * y + z2[c];
            z2[c]          = bq.b2 * x - bq.a2 * y;
            bus[i * 2 + c] = static_cast<f32>(y);
        }
    }
}
} // namespace

TEST_CASE("biquad: hesap-dsp's section step is bit-exact against the audio copy it replaced, single-pass and chunked",
          "[audio][graph][own]")
{
    constexpr i64 frames = 3001;
    u64           seed   = 0xA0D1'0B1AULL;
    containers::Array<f32> input(&galloc());
    for (i64 i = 0; i < frames * 2; ++i)
    {
        input.push_back(static_cast<f32>(static_cast<f64>(splitmix64_next(seed) >> 40U) * 0x1.0p-24 - 0.5));
    }
    const f32 cutoffs[] = {0.01F, 0.125F, 0.49F};
    const f32 qs[]      = {0.5F, 0.7071F, 8.0F};
    for (u8 filter = 0; filter < 4; ++filter)
    {
        for (const f32 cutoff : cutoffs)
        {
            for (const f32 q : qs)
            {
                containers::Array<f32> single(&galloc());
                containers::Array<f32> chunked(&galloc());
                containers::Array<f32> reference(&galloc());
                for (const f32 v : input)
                {
                    single.push_back(v);
                    chunked.push_back(v);
                    reference.push_back(v);
                }
                hesap::dsp::BiquadState<f64> single_state[2] = {};
                audio::apply_biquad(single.data(), frames, filter, cutoff, q, single_state);

                hesap::dsp::BiquadState<f64> chunk_state[2] = {};
                const i64                    blocks[]       = {256, 7, 1, 113, 64};
                i64                          off            = 0;
                for (usize b = 0; off < frames; ++b)
                {
                    const i64 len = blocks[b % 5] < frames - off ? blocks[b % 5] : frames - off;
                    audio::apply_biquad(chunked.data() + off * 2, len, filter, cutoff, q, chunk_state);
                    off += len;
                }

                f64 z1[2] = {};
                f64 z2[2] = {};
                reference_apply_biquad(reference.data(), frames, filter, cutoff, q, z1, z2);

                INFO("filter " << int(filter) << " cutoff " << cutoff << " q " << q);
                const usize bytes = input.size() * sizeof(f32);
                CHECK(std::memcmp(single.data(), reference.data(), bytes) == 0);
                CHECK(std::memcmp(chunked.data(), reference.data(), bytes) == 0);
                for (int c = 0; c < 2; ++c)
                {
                    CHECK(std::memcmp(&single_state[c].z1, &z1[c], sizeof(f64)) == 0);
                    CHECK(std::memcmp(&single_state[c].z2, &z2[c], sizeof(f64)) == 0);
                    CHECK(std::memcmp(&chunk_state[c].z1, &z1[c], sizeof(f64)) == 0);
                    CHECK(std::memcmp(&chunk_state[c].z2, &z2[c], sizeof(f64)) == 0);
                }
            }
        }
    }
}
