// DIAG.10c -- the sanity probes at the audio boundary (crd/audio/audio_boundary.hpp). A source binding is judged by its
// shape (a whole number of frames) and its declared sample rate against the graph's; both renderers refuse what does
// not render, and an undeclared rate still renders, unchanged. A non-finite sample reaching the bus is caught by the
// sample domain. One render period runs its callback in the declared numerical mode, keeps a mode the callback sets
// inside the period, reports both, probes what it wrote, and lays stereo into a device buffer of more channels.
// No device is needed. Every expected value is written by hand. ASCII test names.

#include <crd/audio/audio_boundary.hpp>
#include <crd/audio/audio_graph.hpp>
#include <crd/audio/audio_resources.hpp>
#include <crd/audio/ceir_audio_render.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/audio_ops.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>
#include <crd/core/fp_environment.hpp>
#include <crd/core/value_domain.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace crd; // NOLINT(google-build-using-namespace)
using crd::audio::SourceBindingVerdict;
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
constexpr u32 kNaNBits = 0x7FC00000U;

f32 nan_f32()
{
    f32 f = 0.0F;
    std::memcpy(&f, &kNaNBits, sizeof f);
    return f;
}

void fill_stereo(containers::Array<f32>& a, u32 frames)
{
    a.resize(static_cast<usize>(frames) * 2U, 0.0F);
    for (u32 f = 0; f < frames; ++f)
    {
        a[static_cast<usize>(f) * 2U]      = 0.25F;
        a[static_cast<usize>(f) * 2U + 1U] = -0.25F;
    }
}

// source -> gain{0 dB}, rendered at `rate`
audio::AudioGraphResource source_gain_graph(memory::IAllocator* alloc, u32 rate, u8 loop)
{
    audio::AudioGraphResource g(alloc);
    g.sample_rate = rate;
    g.out_node    = 1U;
    audio::AudioNodeRec src;
    src.type     = static_cast<u8>(audio::AudioNodeType::Source);
    src.name_off = g.intern("s");
    src.loop     = loop;
    g.nodes.push_back(src);
    audio::AudioNodeRec gain;
    gain.type = static_cast<u8>(audio::AudioNodeType::Gain);
    g.nodes.push_back(gain);
    g.edges.push_back({0U, 1U});
    return g;
}

audio::GraphSourceBinding binding(const containers::Array<f32>& samples, u32 channels, u32 rate)
{
    audio::GraphSourceBinding b;
    b.samples     = ConstSpan<f32>(samples.data(), samples.size());
    b.channels    = channels;
    b.sample_rate = rate;
    return b;
}

// A main() holding source@s -> gain{0}; `rate` > 0 sets the func's sample_rate attribute.
ceir::Module* source_gain_module(ceir::Context& ctx, i64 rate)
{
    ceir::Module* const m   = ctx.create_module();
    ceir::Block*        top = m->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m->body()->append(top);
    }
    ceir::Operation* const fn = ceir::func::create_func(ctx, *m, "main", ceir::Visibility::Public, 0U);
    top->append(fn);
    if (rate > 0)
    {
        ctx.set_attr(fn, StringView("sample_rate"), ctx.attr_int(rate));
    }
    ceir::Block* const body  = ceir::func::func_body_block(fn);
    const ceir::TypeId dims[2] = {ctx.type_dim_dynamic(), ctx.type_dim_static(2U)};
    const ceir::TypeId tt      = ctx.type_tensor(ctx.type_f32(), ctx.type_shape(ConstSpan<ceir::TypeId>(dims, 2U)));
    ceir::Operation* const s =
        ceir::audio::build_source(ctx, ctx.attr_symbol(StringView("s")), ctx.attr_int(0), ctx.attr_int(0), tt);
    body->append(s);
    body->append(ceir::audio::build_gain(ctx, s->result(0U), ctx.attr_float(0.0), tt));
    return m;
}

// A period callback that records the mode it ran in and writes a ramp: L = frame, R = -frame.
struct PeriodProbe
{
    FpEnvironment seen{};
    u32           calls     = 0U;
    bool          set_ftz   = false;
    i64           nan_frame = -1;
};

void ramp_callback(void* user, f32* out, u32 frames, u32 /*rate*/)
{
    auto* p = static_cast<PeriodProbe*>(user);
    ++p->calls;
    p->seen = read_fp_environment();
    for (u32 f = 0; f < frames; ++f)
    {
        out[f * 2U]      = static_cast<f32>(f);
        out[f * 2U + 1U] = -static_cast<f32>(f);
    }
    if (p->nan_frame >= 0)
    {
        out[static_cast<usize>(p->nan_frame) * 2U + 1U] = nan_f32();
    }
    if (p->set_ftz)
    {
        FpEnvironment e = read_fp_environment();
        e.mxcsr |= kMxcsrFlushToZero;
        write_fp_environment(e);
    }
}

u32 on_x64(FpDifference d)
{
#if CRD_ARCH_X64
    return static_cast<u32>(d);
#else
    (void)d;
    return 0U; // nothing is read on this architecture
#endif
}
} // namespace

TEST_CASE("diag10c: a source binding is judged by its shape and its declared rate", "[audio][diag10c]")
{
    memory::GrowableTlsfAllocator root;
    containers::Array<f32>        four(&root);
    fill_stereo(four, 2U); // 4 samples: two stereo frames
    containers::Array<f32> three(&root);
    three.resize(3U, 0.0F);
    containers::Array<f32> one(&root);
    one.resize(1U, 0.0F);
    containers::Array<f32> none(&root);

    CHECK(audio::check_source_binding(binding(four, 2U, 48000U), 48000U) == SourceBindingVerdict::Bound);
    CHECK(audio::check_source_binding(binding(four, 2U, 0U), 48000U) == SourceBindingVerdict::RateUndeclared);
    CHECK(audio::check_source_binding(binding(four, 2U, 44100U), 48000U) == SourceBindingVerdict::RateMismatch);
    CHECK(audio::check_source_binding(binding(three, 2U, 48000U), 48000U) == SourceBindingVerdict::PartialFrame);
    CHECK(audio::check_source_binding(binding(one, 2U, 48000U), 48000U) == SourceBindingVerdict::PartialFrame);
    CHECK(audio::check_source_binding(binding(none, 2U, 48000U), 48000U) == SourceBindingVerdict::Unbound);
    CHECK(audio::check_source_binding(binding(four, 0U, 48000U), 48000U) == SourceBindingVerdict::Unbound);
    // The shape is judged before the rate: a ragged buffer at the wrong rate is a partial frame first.
    CHECK(audio::check_source_binding(binding(three, 2U, 44100U), 48000U) == SourceBindingVerdict::PartialFrame);
    CHECK(audio::source_binding_renders(SourceBindingVerdict::Bound));
    CHECK(audio::source_binding_renders(SourceBindingVerdict::RateUndeclared));
    CHECK_FALSE(audio::source_binding_renders(SourceBindingVerdict::RateMismatch));
    CHECK(std::strcmp(audio::source_binding_verdict_name(SourceBindingVerdict::RateMismatch), "rate-mismatch") == 0);
}

TEST_CASE("diag10c: render_graph refuses a rate mismatch and a partial frame", "[audio][diag10c]")
{
    memory::GrowableTlsfAllocator   root;
    const audio::AudioGraphResource g = source_gain_graph(&root, 48000U, 0U);
    containers::Array<f32>          buf(&root);
    fill_stereo(buf, 64U);

    containers::Array<f32>    declared(&root);
    containers::Array<f32>    undeclared(&root);
    audio::GraphSourceBinding b[2] = {binding(buf, 2U, 48000U), {}};
    REQUIRE(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 32, declared) == 32);
    b[0].sample_rate = 0U; // undeclared: still renders, byte for byte as before
    REQUIRE(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 32, undeclared) == 32);
    CHECK(std::memcmp(declared.data(), undeclared.data(), declared.size() * sizeof(f32)) == 0);

    // A 44.1 kHz buffer in a 48 kHz graph would render at the wrong pitch and length: refused.
    containers::Array<f32> out(&root);
    b[0].sample_rate = 44100U;
    CHECK(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 32, out) == 0);
    CHECK(out.size() == 0U);

    // 129 samples for 2 channels: the trailing half frame was silently dropped; now refused.
    containers::Array<f32> ragged(&root);
    fill_stereo(ragged, 64U);
    ragged.push_back(0.5F);
    b[0] = binding(ragged, 2U, 48000U);
    CHECK(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 32, out) == 0);
}

TEST_CASE("diag10c: a looped source of less than one frame is refused and never divides by zero", "[audio][diag10c]")
{
    memory::GrowableTlsfAllocator   root;
    const audio::AudioGraphResource g = source_gain_graph(&root, 48000U, 1U);
    containers::Array<f32>          half(&root);
    half.resize(1U, 0.5F); // one sample for two channels: zero whole frames
    audio::GraphSourceBinding b[2] = {binding(half, 2U, 0U), {}};
    containers::Array<f32>    out(&root);
    CHECK(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 8, out) == 0);
}

TEST_CASE("diag10c: the CEIR audio executor checks bindings against the graph's sample_rate", "[audio][ceir][diag10c]")
{
    memory::GrowableTlsfAllocator root;
    ceir::Context                 ctx(&root);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::audio::register_audio_ops(ctx);
    containers::Array<f32> buf(&root);
    fill_stereo(buf, 64U);

    audio::CeirSourceBinding cb[1];
    cb[0].name    = StringView("s");
    cb[0].binding = binding(buf, 2U, 44100U);
    containers::Array<f32> out(&root);

    ceir::Module* const at44 = source_gain_module(ctx, 44100);
    CHECK(audio::execute_audio_graph_ceir(ctx, *at44, ConstSpan<audio::CeirSourceBinding>(cb, 1U), 16, out) == 16);
    // No sample_rate attribute means 48 kHz (the AudioGraphResource default): a 44.1 kHz buffer does not match.
    ceir::Module* const implicit = source_gain_module(ctx, 0);
    CHECK(audio::execute_audio_graph_ceir(ctx, *implicit, ConstSpan<audio::CeirSourceBinding>(cb, 1U), 16, out) == 0);
    cb[0].binding.sample_rate = 0U;
    CHECK(audio::execute_audio_graph_ceir(ctx, *implicit, ConstSpan<audio::CeirSourceBinding>(cb, 1U), 16, out) == 16);
    // A ragged buffer is refused here too.
    containers::Array<f32> ragged(&root);
    fill_stereo(ragged, 8U);
    ragged.push_back(0.0F);
    cb[0].binding = binding(ragged, 2U, 48000U);
    CHECK(audio::execute_audio_graph_ceir(ctx, *implicit, ConstSpan<audio::CeirSourceBinding>(cb, 1U), 4, out) == 0);
}

TEST_CASE("diag10c: a non-finite sample reaching the bus is caught by the sample domain", "[audio][diag10c]")
{
    memory::GrowableTlsfAllocator   root;
    const audio::AudioGraphResource g = source_gain_graph(&root, 48000U, 0U);
    containers::Array<f32>          buf(&root);
    fill_stereo(buf, 16U);
    audio::GraphSourceBinding b[2] = {binding(buf, 2U, 48000U), {}};
    containers::Array<f32>    out(&root);
    REQUIRE(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 16, out) == 16);
    CHECK(probe_values(audio::audio_sample_domain(), out.data(), out.size()).ok());

    buf[5U * 2U + 1U] = nan_f32(); // frame 5, right channel
    REQUIRE(audio::render_graph(g, ConstSpan<audio::GraphSourceBinding>(b, 2U), 16, out) == 16);
    const ValueDomainReport r = probe_values(audio::audio_sample_domain(), out.data(), out.size());
    CHECK(r.violating == 1U);
    CHECK(r.first_index == 11U);
    CHECK(r.first_violations == static_cast<u32>(ValueViolation::NaN));
    CHECK(std::strcmp(audio::audio_sample_domain().name, "audio-sample") == 0);
}

TEST_CASE("diag10c: a render period runs its callback in the declared mode and keeps the callback's mode inside it",
          "[audio][realtime][diag10c][fp]")
{
    const FpEnvironment declared = declared_fp_environment();
    f32                 device[16 * 2] = {};
    PeriodProbe         p;

    // The device thread in the declared mode: nothing diverges, the ramp lands as written.
    audio::AudioPeriodReport r = audio::render_audio_period(&ramp_callback, &p, device, 16U, 2U, 48000U, nullptr, 0U);
    REQUIRE(r.called);
    CHECK(r.entry_fp_difference == 0U);
    CHECK(r.exit_fp_difference == 0U);
    CHECK(r.samples.ok());
    CHECK(r.samples.checked == 32U);
    CHECK(device[2 * 3] == 3.0F);
    CHECK(device[2 * 3 + 1] == -3.0F);

    // A device thread left in round-down: the callback still runs in the declared mode, the entry is reported, and the
    // thread's own mode comes back.
    {
        FpEnvironment down = declared;
        down.mxcsr = (down.mxcsr & ~kMxcsrRoundingMask) | (static_cast<u32>(FpRounding::Down) << kMxcsrRoundingShift);
        const ScopedFpEnvironment scoped(down);
        const FpEnvironment       thread_mode = read_fp_environment();
        r = audio::render_audio_period(&ramp_callback, &p, device, 16U, 2U, 48000U, nullptr, 0U);
        CHECK(r.entry_fp_difference == on_x64(FpDifference::Rounding));
        CHECK(fp_differences(p.seen, declared) == 0U);
        CHECK(fp_differences(read_fp_environment(), thread_mode) == 0U);
    }

    // A callback that leaves flush-to-zero set: reported at exit, and the next period starts clean.
    p.set_ftz = true;
    r         = audio::render_audio_period(&ramp_callback, &p, device, 16U, 2U, 48000U, nullptr, 0U);
    CHECK(r.exit_fp_difference == on_x64(FpDifference::FlushToZero));
    CHECK(fp_differences(read_fp_environment(), declared) == 0U);
    p.set_ftz = false;
    r         = audio::render_audio_period(&ramp_callback, &p, device, 16U, 2U, 48000U, nullptr, 0U);
    CHECK(r.entry_fp_difference == 0U);
    CHECK(r.exit_fp_difference == 0U);

    // A NaN the callback writes is named by its stereo index.
    p.nan_frame = 3;
    r           = audio::render_audio_period(&ramp_callback, &p, device, 16U, 2U, 48000U, nullptr, 0U);
    CHECK(r.samples.violating == 1U);
    CHECK(r.samples.first_index == 7U);
    CHECK(r.samples.first_violations == static_cast<u32>(ValueViolation::NaN));
}

TEST_CASE("diag10c: a render period lays stereo into a wider device buffer and stays silent when it cannot run",
          "[audio][realtime][diag10c]")
{
    PeriodProbe p;
    f32         device[8 * 6];
    f32         scratch[8 * 2];
    for (f32& v : device)
    {
        v = 9.0F; // stale data from the previous period
    }

    // A six-channel endpoint: the callback's stereo goes to channels 0 and 1, the other four are silent.
    audio::AudioPeriodReport r = audio::render_audio_period(&ramp_callback, &p, device, 8U, 6U, 48000U, scratch, 16U);
    REQUIRE(r.called);
    CHECK(r.samples.checked == 16U);
    bool laid_out = true;
    for (u32 f = 0; f < 8U; ++f)
    {
        laid_out = laid_out && device[f * 6U] == static_cast<f32>(f) && device[f * 6U + 1U] == -static_cast<f32>(f);
        for (u32 c = 2; c < 6U; ++c)
        {
            laid_out = laid_out && device[f * 6U + c] == 0.0F;
        }
    }
    CHECK(laid_out);

    // Too small a scratch, a mono endpoint or no callback: silence, and the callback is not called.
    for (f32& v : device)
    {
        v = 9.0F;
    }
    const u32 calls = p.calls;
    CHECK_FALSE(audio::render_audio_period(&ramp_callback, &p, device, 8U, 6U, 48000U, scratch, 15U).called);
    CHECK_FALSE(audio::render_audio_period(&ramp_callback, &p, device, 8U, 6U, 48000U, nullptr, 0U).called);
    CHECK_FALSE(audio::render_audio_period(&ramp_callback, &p, device, 8U, 1U, 48000U, scratch, 16U).called);
    CHECK_FALSE(audio::render_audio_period(nullptr, &p, device, 8U, 2U, 48000U, nullptr, 0U).called);
    CHECK(p.calls == calls);
    bool silent = true;
    for (u32 i = 0; i < 8U * 2U; ++i)
    {
        silent = silent && device[i] == 0.0F;
    }
    CHECK(silent);
}
