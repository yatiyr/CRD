// test_timeline.cpp — GEO-9 (D-007 row 74): the TIMELINE resource gates.
//  · builder → loader round-trip (byte contracts; ONE validator refuses at both ends — every refusal class)
//  · deterministic evaluation: hand-oracle item positions, THE TRANSITION WINDOW (weights sum to 1, media
//    handles extend past trims), gaps, time warps, cross-rate audio (samples vs frames — exact)
//  · automation on the ONE curve engine: Step/Linear/CubicHermite hand oracles, boundary clamps, and
//    rational-EXACT segment selection where f64 seconds could not distinguish
//  · THE RENDER GATE: 2 takes + a centered dissolve + an audio track + an automated camera param → a
//    deterministic EXR sequence through OUR codec — decoded pixels equal the hand-computed mix BIT-EXACT,
//    and two renders produce IDENTICAL bytes.

#include <crd/hesap/interp/keyframe.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/hdr_image.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/timeline/timeline_eval.hpp>
#include <crd/timeline/timeline_render.hpp>
#include <crd/timeline/timeline_resource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <utility>

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(256U << 20U);
    return a;
}

constexpr time::RationalRate k24 = time::kRate24;

time::RationalTime rt(i64 v, time::RationalRate r = k24)
{
    return {v, r};
}

time::TimeRange range(i64 start, i64 dur, time::RationalRate r = k24)
{
    return {rt(start, r), rt(dur, r)};
}

// ── THE GATE TIMELINE ──────────────────────────────────────────────────────────────────────────────────────────
// V1 (video): takeA [media 100..148 @24] · dissolve (in 12, out 12) · takeB [media 200..248 @24]
// A1 (audio): music clip in SAMPLES (48000/1) — 4 s
// automation: camera.fov CubicHermite 40→60 over 4 s (tangents 5/s) · light.intensity Linear 1→3 over 1 s ·
//             shot.index Step 0→1 at the cut
timeline::TimelineResource make_gate_timeline()
{
    timeline::TimelineResource tl(&galloc());
    tl.name_off = tl.intern("geo9-gate");

    // media
    timeline::MediaRec media_a;
    media_a.kind     = static_cast<u8>(timeline::MediaKind::External);
    media_a.name_off = tl.intern("takeA");
    media_a.url_off  = tl.intern("file:///takes/takeA/frame.%04d.exr");
    tl.media.push_back(media_a);
    timeline::MediaRec media_b;
    media_b.kind     = static_cast<u8>(timeline::MediaKind::External);
    media_b.name_off = tl.intern("takeB");
    media_b.url_off  = tl.intern("file:///takes/takeB/frame.%04d.exr");
    tl.media.push_back(media_b);
    timeline::MediaRec media_music;
    media_music.kind     = static_cast<u8>(timeline::MediaKind::Missing);
    media_music.name_off = tl.intern("music");
    tl.media.push_back(media_music);

    // markers: one on takeA, one timeline-level
    timeline::MarkerRec sync;
    sync.name_off  = tl.intern("SYNC");
    sync.color_off = tl.intern("RED");
    sync.range     = range(10, 1);
    tl.markers.push_back(sync);
    timeline::MarkerRec note;
    note.name_off  = tl.intern("review");
    note.color_off = tl.intern("GREEN");
    note.range     = range(0, 96);
    tl.markers.push_back(note);
    tl.first_marker = 1;
    tl.marker_count = 1;

    // V1 items
    timeline::ItemRec take_a;
    take_a.type             = static_cast<u8>(timeline::ItemType::Clip);
    take_a.name_off         = tl.intern("takeA");
    take_a.has_source_range = 1;
    take_a.source_range     = range(100, 48);
    take_a.media_ref        = 0;
    take_a.first_marker     = 0;
    take_a.marker_count     = 1;
    tl.items.push_back(take_a);

    timeline::ItemRec dissolve;
    dissolve.type                = static_cast<u8>(timeline::ItemType::Transition);
    dissolve.name_off            = tl.intern("cut");
    dissolve.transition_type_off = tl.intern("SMPTE_Dissolve");
    dissolve.in_offset           = rt(12);
    dissolve.out_offset          = rt(12);
    tl.items.push_back(dissolve);

    timeline::ItemRec take_b;
    take_b.type             = static_cast<u8>(timeline::ItemType::Clip);
    take_b.name_off         = tl.intern("takeB");
    take_b.has_source_range = 1;
    take_b.source_range     = range(200, 48);
    take_b.media_ref        = 1;
    tl.items.push_back(take_b);

    timeline::TrackRec video;
    video.kind          = static_cast<u8>(timeline::TrackKind::Video);
    video.kind_name_off = tl.intern("Video");
    video.name_off      = tl.intern("V1");
    video.first_item    = 0;
    video.item_count    = 3;
    tl.tracks.push_back(video);

    // A1: one audio clip in SAMPLES
    timeline::ItemRec music;
    music.type             = static_cast<u8>(timeline::ItemType::Clip);
    music.name_off         = tl.intern("music");
    music.has_source_range = 1;
    music.source_range     = range(0, 192000, time::make_rate(48000, 1)); // 4 s of samples
    music.media_ref        = 2;
    tl.items.push_back(music);

    timeline::TrackRec audio;
    audio.kind          = static_cast<u8>(timeline::TrackKind::Audio);
    audio.kind_name_off = tl.intern("Audio");
    audio.name_off      = tl.intern("A1");
    audio.first_item    = 3;
    audio.item_count    = 1;
    tl.tracks.push_back(audio);

    // automation
    timeline::AutomationRec fov;
    fov.target_off = tl.intern("camera.fov");
    fov.rate       = k24;
    fov.interp     = static_cast<u8>(hesap::interp::KeyInterp::CubicHermite);
    fov.key_count  = 2;
    fov.ticks_off  = 0;
    fov.values_off = 0;
    tl.auto_ticks.push_back(0);
    tl.auto_ticks.push_back(96);
    const f32 fov_vals[6] = {5.0F, 40.0F, 5.0F, 5.0F, 60.0F, 5.0F}; // [in·value·out] per key, tangents /second
    for (f32 v : fov_vals) { tl.auto_values.push_back(v); }
    tl.automation.push_back(fov);

    timeline::AutomationRec intensity;
    intensity.target_off = tl.intern("light.intensity");
    intensity.rate       = k24;
    intensity.interp     = static_cast<u8>(hesap::interp::KeyInterp::Linear);
    intensity.key_count  = 2;
    intensity.ticks_off  = 2;
    intensity.values_off = 6;
    tl.auto_ticks.push_back(0);
    tl.auto_ticks.push_back(24);
    tl.auto_values.push_back(1.0F);
    tl.auto_values.push_back(3.0F);
    tl.automation.push_back(intensity);

    timeline::AutomationRec shot;
    shot.target_off = tl.intern("shot.index");
    shot.rate       = k24;
    shot.interp     = static_cast<u8>(hesap::interp::KeyInterp::Step);
    shot.key_count  = 2;
    shot.ticks_off  = 4;
    shot.values_off = 8;
    tl.auto_ticks.push_back(0);
    tl.auto_ticks.push_back(48);
    tl.auto_values.push_back(0.0F);
    tl.auto_values.push_back(1.0F);
    tl.automation.push_back(shot);

    return tl;
}

resources::ResourceId gate_id()
{
    return {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
}

} // namespace

TEST_CASE("timeline build -> load round-trips every table", "[timeline]")
{
    const timeline::TimelineResource   tl    = make_gate_timeline();
    containers::Array<u8>              bytes = timeline::timeline_build(tl, gate_id(), &galloc());
    REQUIRE(bytes.size() > 0);

    timeline::TimelineLoader loader(&galloc());
    resources::LoadContext   ctx;
    ctx.id        = gate_id();
    ctx.bytes     = containers::as_const_span(bytes);
    ctx.allocator = &galloc();
    auto* loaded  = static_cast<timeline::TimelineResource*>(loader.load(ctx));
    REQUIRE(loaded != nullptr);

    CHECK(std::strcmp(loaded->name(), "geo9-gate") == 0);
    REQUIRE(loaded->tracks.size() == 2);
    REQUIRE(loaded->items.size() == 4);
    REQUIRE(loaded->media.size() == 3);
    REQUIRE(loaded->markers.size() == 2);
    REQUIRE(loaded->automation.size() == 3);
    CHECK(loaded->first_marker == 1);
    CHECK(loaded->marker_count == 1);

    CHECK(std::strcmp(loaded->str(loaded->tracks[0].name_off), "V1") == 0);
    CHECK(loaded->tracks[0].kind == static_cast<u8>(timeline::TrackKind::Video));
    CHECK(loaded->items[1].type == static_cast<u8>(timeline::ItemType::Transition));
    CHECK(loaded->items[1].in_offset.value == 12);
    CHECK(loaded->items[3].source_range.duration.value == 192000);
    CHECK(loaded->items[3].source_range.duration.rate.num == 48000);
    CHECK(std::strcmp(loaded->str(loaded->media[0].url_off), "file:///takes/takeA/frame.%04d.exr") == 0);
    CHECK(std::strcmp(loaded->str(loaded->automation[0].target_off), "camera.fov") == 0);
    CHECK(loaded->auto_ticks.size() == 6);
    CHECK(loaded->auto_values.size() == 10);

    // deterministic serialization: build(load(build(x))) is byte-identical to build(x)
    containers::Array<u8> again = timeline::timeline_build(*loaded, gate_id(), &galloc());
    REQUIRE(again.size() == bytes.size());
    CHECK(std::memcmp(again.data(), bytes.data(), bytes.size()) == 0);

    loader.unload(loaded);
}

TEST_CASE("timeline builder refuses every malformed class", "[timeline]")
{
    const auto build_of = [](const timeline::TimelineResource& tl) {
        return timeline::timeline_build(tl, gate_id(), &galloc()).size();
    };

    { // a transition FIRST in its track
        timeline::TimelineResource tl(&galloc());
        timeline::ItemRec          tr;
        tr.type       = static_cast<u8>(timeline::ItemType::Transition);
        tr.in_offset  = rt(1);
        tr.out_offset = rt(1);
        tl.items.push_back(tr);
        timeline::ItemRec clip;
        clip.type             = static_cast<u8>(timeline::ItemType::Clip);
        clip.has_source_range = 1;
        clip.source_range     = range(0, 10);
        tl.items.push_back(clip);
        timeline::TrackRec t;
        t.item_count = 2;
        tl.tracks.push_back(t);
        CHECK(build_of(tl) == 0);
    }
    { // a clip WITHOUT a resolved range
        timeline::TimelineResource tl(&galloc());
        timeline::ItemRec          clip;
        clip.type = static_cast<u8>(timeline::ItemType::Clip);
        tl.items.push_back(clip);
        timeline::TrackRec t;
        t.item_count = 1;
        tl.tracks.push_back(t);
        CHECK(build_of(tl) == 0);
    }
    { // media_ref out of range
        timeline::TimelineResource tl(&galloc());
        timeline::ItemRec          clip;
        clip.type             = static_cast<u8>(timeline::ItemType::Clip);
        clip.has_source_range = 1;
        clip.source_range     = range(0, 10);
        clip.media_ref        = 5;
        tl.items.push_back(clip);
        timeline::TrackRec t;
        t.item_count = 1;
        tl.tracks.push_back(t);
        CHECK(build_of(tl) == 0);
    }
    { // tracks that do NOT partition the item array
        timeline::TimelineResource tl(&galloc());
        timeline::ItemRec          clip;
        clip.type             = static_cast<u8>(timeline::ItemType::Clip);
        clip.has_source_range = 1;
        clip.source_range     = range(0, 10);
        tl.items.push_back(clip);
        // no track claims the item
        CHECK(build_of(tl) == 0);
    }
    { // automation ticks not strictly increasing
        timeline::TimelineResource tl(&galloc());
        timeline::AutomationRec    a;
        a.target_off = tl.intern("x");
        a.rate       = k24;
        a.interp     = static_cast<u8>(hesap::interp::KeyInterp::Step);
        a.key_count  = 2;
        tl.auto_ticks.push_back(5);
        tl.auto_ticks.push_back(5);
        tl.auto_values.push_back(0.0F);
        tl.auto_values.push_back(1.0F);
        tl.automation.push_back(a);
        CHECK(build_of(tl) == 0);
    }
    { // automation value blob too small for CubicHermite triples
        timeline::TimelineResource tl(&galloc());
        timeline::AutomationRec    a;
        a.target_off = tl.intern("x");
        a.rate       = k24;
        a.interp     = static_cast<u8>(hesap::interp::KeyInterp::CubicHermite);
        a.key_count  = 2;
        tl.auto_ticks.push_back(0);
        tl.auto_ticks.push_back(1);
        for (int i = 0; i < 4; ++i) { tl.auto_values.push_back(0.0F); } // needs 6
        tl.automation.push_back(a);
        CHECK(build_of(tl) == 0);
    }
    { // a corrupt artifact refuses at LOAD (flip a track's first_item after build)
        timeline::TimelineResource tl = make_gate_timeline();
        containers::Array<u8>      bytes = timeline::timeline_build(tl, gate_id(), &galloc());
        REQUIRE(bytes.size() > 0);
        // find 'TMTK' and flip a RANGE after it — whatever mix of header/record fields it hits, the artifact
        // must refuse (either crdr_read or the shared validator; a silent partial load is the only failure)
        bool corrupted = false;
        for (usize i = 0; i + 48 <= bytes.size(); ++i)
        {
            if (std::memcmp(bytes.data() + i, "TMTK", 4) == 0)
            {
                for (usize k = 16; k < 44; ++k) { bytes[i + k] ^= 0x7FU; }
                corrupted = true;
                break;
            }
        }
        REQUIRE(corrupted);
        timeline::TimelineLoader loader(&galloc());
        resources::LoadContext   ctx;
        ctx.id        = gate_id();
        ctx.bytes     = containers::as_const_span(bytes);
        ctx.allocator = &galloc();
        CHECK(loader.load(ctx) == nullptr);
    }
}

TEST_CASE("evaluation: positions, the transition window, handles, cross-rate audio", "[timeline]")
{
    const timeline::TimelineResource tl = make_gate_timeline();

    // durations + starts (rational accumulation)
    CHECK(timeline::track_duration(tl, tl.tracks[0]) == rt(96));
    CHECK(timeline::track_duration(tl, tl.tracks[1]) == rt(192000, time::make_rate(48000, 1))); // = 4 s
    CHECK(timeline::timeline_duration(tl) == rt(96)); // 4 s each — equal across rates
    CHECK(timeline::item_start(tl, tl.tracks[0], 2) == rt(48)); // takeB starts at the cut

    containers::Array<timeline::ActiveClip> active(&galloc());

    // before the window: takeA alone at full weight, media_time = 100 + t
    active.clear();
    timeline::evaluate_tracks(tl, rt(24), active);
    REQUIRE(active.size() == 2); // takeA + audio
    CHECK(active[0].item_index == 0);
    CHECK(active[0].weight == 1.0F);
    CHECK(active[0].media_time == rt(124));
    CHECK(active[1].item_index == 3); // the audio clip
    CHECK(active[1].media_time == rt(48000, time::make_rate(48000, 1))); // 1 s in SAMPLES — exact cross-rate

    // inside the window at t=42: outgoing takeA 0.75 (media 142), incoming takeB 0.25 (media 194 — a HANDLE
    // before its trim start 200)
    active.clear();
    timeline::evaluate_tracks(tl, rt(42), active);
    REQUIRE(active.size() == 3);
    CHECK(active[0].item_index == 0);
    CHECK(active[0].weight == 0.75F);
    CHECK(active[0].media_time == rt(142));
    CHECK(active[1].item_index == 2);
    CHECK(active[1].weight == 0.25F);
    CHECK(active[1].media_time == rt(194));
    CHECK(active[0].weight + active[1].weight == 1.0F);

    // at the cut exactly: 0.5 / 0.5; takeA's media extends past its trim end (a tail HANDLE)
    active.clear();
    timeline::evaluate_tracks(tl, rt(48), active);
    REQUIRE(active.size() == 3);
    CHECK(active[0].weight == 0.5F);
    CHECK(active[1].weight == 0.5F);
    CHECK(active[0].media_time == rt(148)); // == trim end — the first handle frame
    CHECK(active[1].media_time == rt(200));

    // after the window: takeB alone on video (audio still runs — 2.5 s of its 4 s)
    active.clear();
    timeline::evaluate_tracks(tl, rt(60), active);
    REQUIRE(active.size() == 2);
    CHECK(active[0].item_index == 2);
    CHECK(active[0].weight == 1.0F);
    CHECK(active[0].media_time == rt(212));
    CHECK(active[1].item_index == 3);

    // past the video track's end (96) but inside audio's 4 s? equal ends — nothing anywhere
    active.clear();
    timeline::evaluate_tracks(tl, rt(96), active);
    CHECK(active.size() == 0); // both tracks end at 4 s; the end is EXCLUSIVE

    // negative time: nothing
    active.clear();
    timeline::evaluate_tracks(tl, rt(-1), active);
    CHECK(active.size() == 0);
}

TEST_CASE("evaluation: gaps show nothing, time warps scale media time exactly", "[timeline]")
{
    timeline::TimelineResource tl(&galloc());

    timeline::ItemRec gap;
    gap.type             = static_cast<u8>(timeline::ItemType::Gap);
    gap.has_source_range = 1;
    gap.source_range     = range(0, 24);
    tl.items.push_back(gap);

    timeline::ItemRec warped;
    warped.type             = static_cast<u8>(timeline::ItemType::Clip);
    warped.name_off         = tl.intern("warped");
    warped.has_source_range = 1;
    warped.source_range     = range(100, 48);
    warped.first_effect     = 0;
    warped.effect_count     = 1;
    tl.items.push_back(warped);

    timeline::EffectRec warp;
    warp.type        = static_cast<u8>(timeline::EffectType::LinearTimeWarp);
    warp.time_scalar = 2.0; // double speed
    tl.effects.push_back(warp);

    timeline::ItemRec frozen;
    frozen.type             = static_cast<u8>(timeline::ItemType::Clip);
    frozen.name_off         = tl.intern("frozen");
    frozen.has_source_range = 1;
    frozen.source_range     = range(300, 24);
    frozen.first_effect     = 1;
    frozen.effect_count     = 1;
    tl.items.push_back(frozen);

    timeline::EffectRec freeze;
    freeze.type = static_cast<u8>(timeline::EffectType::FreezeFrame);
    tl.effects.push_back(freeze);

    timeline::TrackRec video;
    video.kind       = static_cast<u8>(timeline::TrackKind::Video);
    video.item_count = 3;
    tl.tracks.push_back(video);

    containers::Array<timeline::ActiveClip> active(&galloc());

    // inside the gap: nothing
    timeline::evaluate_tracks(tl, rt(10), active);
    CHECK(active.size() == 0);

    // the warped clip runs [24, 72): at t=30 (6 into the clip) → media 100 + 6*2 = 112, EXACT rational
    active.clear();
    timeline::evaluate_tracks(tl, rt(30), active);
    REQUIRE(active.size() == 1);
    CHECK(active[0].media_time == rt(112));

    // an odd offset stays exact: t=31 → 100 + 7*2 = 114
    active.clear();
    timeline::evaluate_tracks(tl, rt(31), active);
    REQUIRE(active.size() == 1);
    CHECK(active[0].media_time == rt(114));

    // the frozen clip runs [72, 96): media time pins to 300 everywhere inside
    active.clear();
    timeline::evaluate_tracks(tl, rt(80), active);
    REQUIRE(active.size() == 1);
    CHECK(active[0].media_time == rt(300));
    active.clear();
    timeline::evaluate_tracks(tl, rt(95), active);
    REQUIRE(active.size() == 1);
    CHECK(active[0].media_time == rt(300));
}

TEST_CASE("automation: the ONE curve engine's semantics + rational-exact segment picks", "[timeline]")
{
    const timeline::TimelineResource tl = make_gate_timeline();
    f32                              v  = 0.0F;

    const u32 fov = timeline::find_automation(tl, "camera.fov");
    REQUIRE(fov != timeline::kInvalidIndex);

    // boundary clamps
    REQUIRE(timeline::automation_value(tl, fov, rt(-10), v));
    CHECK(v == 40.0F);
    REQUIRE(timeline::automation_value(tl, fov, rt(1000), v));
    CHECK(v == 60.0F);

    // the Hermite hand oracle: u=1/2 over 4 s with both tangents 5/s → 20 + 2.5 + 30 − 2.5 = 50
    REQUIRE(timeline::automation_value(tl, fov, rt(48), v));
    CHECK(v == 50.0F);

    const u32 li = timeline::find_automation(tl, "light.intensity");
    REQUIRE(timeline::automation_value(tl, li, rt(12), v));
    CHECK(v == 2.0F); // linear midpoint

    const u32 shot = timeline::find_automation(tl, "shot.index");
    REQUIRE(timeline::automation_value(tl, shot, rt(47), v));
    CHECK(v == 0.0F);
    REQUIRE(timeline::automation_value(tl, shot, rt(48), v)); // AT the key — exact rational tie
    CHECK(v == 1.0F);
    // the same instant expressed on a FINER grid still lands on the key exactly
    REQUIRE(timeline::automation_value(tl, shot, rt(48 * 1000, time::make_rate(24000, 1)), v));
    CHECK(v == 1.0F);
    // one fine tick EARLIER stays in the previous segment — f64 seconds could not reliably say this
    REQUIRE(timeline::automation_value(tl, shot, rt(48 * 1000 - 1, time::make_rate(24000, 1)), v));
    CHECK(v == 0.0F);

    CHECK(timeline::find_automation(tl, "no.such.param") == timeline::kInvalidIndex);
    CHECK_FALSE(timeline::automation_value(tl, 99, rt(0), v));
}

namespace
{

struct TakeResolver
{
    f32 color_a[3] = {1.0F, 0.25F, 0.0F};
    f32 color_b[3] = {0.0F, 0.5F, 1.0F};
};

bool resolve_take(void* user, const timeline::TimelineResource& tl, const timeline::ActiveClip& clip,
                  resources::HdrImage& out)
{
    auto*       res  = static_cast<TakeResolver*>(user);
    const auto& item = tl.items[clip.item_index];
    const char* name = tl.str(item.name_off);
    const f32*  c    = nullptr;
    if (std::strcmp(name, "takeA") == 0) { c = res->color_a; }
    else if (std::strcmp(name, "takeB") == 0) { c = res->color_b; }
    if (c == nullptr) { return false; }
    for (u32 y = 0; y < out.height; ++y)
    {
        for (u32 x = 0; x < out.width; ++x)
        {
            out.at(x, y, 0) = c[0];
            out.at(x, y, 1) = c[1];
            out.at(x, y, 2) = c[2];
        }
    }
    return true;
}

struct FrameCapture
{
    containers::Array<containers::Array<u8>>* frames;
    memory::IAllocator*                       alloc;
};

bool capture_frame(void* user, i64 frame_index, containers::ConstSpan<u8> exr_bytes)
{
    auto* cap = static_cast<FrameCapture*>(user);
    (void)frame_index;
    containers::Array<u8> copy(cap->alloc);
    for (usize i = 0; i < exr_bytes.size(); ++i) { copy.push_back(exr_bytes[i]); }
    cap->frames->push_back(std::move(copy));
    return true;
}

} // namespace

TEST_CASE("THE RENDER GATE: dissolve to EXR sequence, bit-exact and deterministic", "[timeline][render]")
{
    const timeline::TimelineResource tl = make_gate_timeline();

    timeline::RenderConfig cfg;
    cfg.frame_rate  = k24;
    cfg.start       = rt(0);
    cfg.frame_count = 96;
    cfg.width       = 8;
    cfg.height      = 8;
    cfg.pixel_type  = resources::ExrPixelType::Float; // f32 verbatim — the bit-exact pixel gate
    cfg.compression = resources::ExrCompression::Zip;

    TakeResolver resolver;

    containers::Array<containers::Array<u8>> frames(&galloc());
    FrameCapture                             cap{&frames, &galloc()};
    const i64 done = timeline::render_exr_sequence(tl, cfg, &resolve_take, &resolver, &capture_frame, &cap,
                                                   &galloc());
    REQUIRE(done == 96);
    REQUIRE(frames.size() == 96);

    // decode + verify against the HAND-COMPUTED mix (same f32 op order as the driver: acc += w * c)
    const auto check_frame = [&](usize f, f32 wa, f32 wb) {
        resources::HdrImage img(&galloc());
        REQUIRE(resources::hdr_decode_exr(containers::as_const_span(frames[f]), img, &galloc()) ==
                resources::HdrError::Ok);
        REQUIRE(img.width == 8);
        REQUIRE(img.height == 8);
        REQUIRE(img.channels == 3);
        // the SAME f32 op order as the driver: acc starts at 0, += w·c per active clip (A first, then B)
        f32                expected[3] = {0.0F, 0.0F, 0.0F};
        const TakeResolver r;
        for (int c = 0; c < 3; ++c)
        {
            expected[c] += wa * r.color_a[c];
            expected[c] += wb * r.color_b[c];
        }
        for (int c = 0; c < 3; ++c)
        {
            CAPTURE(f, c, wa, wb);
            REQUIRE(img.at(4, 4, static_cast<u32>(c)) == expected[c]);
        }
    };

    check_frame(0, 1.0F, 0.0F);   // pure takeA
    check_frame(24, 1.0F, 0.0F);  // still pure takeA
    check_frame(35, 1.0F, 0.0F);  // last frame before the window
    check_frame(42, 0.75F, 0.25F); // inside the dissolve (6/24 — exact in binary)
    check_frame(48, 0.5F, 0.5F);   // the cut — half and half
    check_frame(57, 0.125F, 0.875F); // 21/24 — exact in binary
    check_frame(60, 0.0F, 1.0F);  // window closed — pure takeB
    check_frame(95, 0.0F, 1.0F);

    // frame 36: the window OPENS (incoming weight 0 — takeB contributes zero but takeA still fills)
    check_frame(36, 1.0F, 0.0F);

    // determinism: a second render is byte-identical, frame for frame
    containers::Array<containers::Array<u8>> frames2(&galloc());
    FrameCapture                             cap2{&frames2, &galloc()};
    REQUIRE(timeline::render_exr_sequence(tl, cfg, &resolve_take, &resolver, &capture_frame, &cap2,
                                          &galloc()) == 96);
    REQUIRE(frames2.size() == frames.size());
    for (usize f = 0; f < frames.size(); ++f)
    {
        REQUIRE(frames2[f].size() == frames[f].size());
        REQUIRE(std::memcmp(frames2[f].data(), frames[f].data(), frames[f].size()) == 0);
    }
}
