// tests/cooker/test_timeline_cook.cpp — GEO-9: the `.otio` cook handler gate. A real NLE-shaped .otio file
// cooks through the REAL handler into a 'TIML' artifact, loads through the REAL loader, and the editorial
// structure survives EXACTLY (NTSC rationals, the dissolve, the audio samples, the resolved clip trim).
// Determinism: cooking twice is byte-identical (the GEO-6 incremental contract). A contradictory .otio
// (a clip with neither source_range nor available_range) refuses with ok=false.

#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/timeline/timeline_eval.hpp>
#include <crd/timeline/timeline_resource.hpp>

#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
void register_otio_timeline_handler(); // defined in timeline_otio.cpp
}

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

crd::cooker::CookHandlerFn otio_handler()
{
    static bool registered = false;
    if (!registered)
    {
        crd::cooker::register_otio_timeline_handler();
        registered = true;
    }
    return crd::cooker::find_cook_handler(crd::containers::StringView(".otio"));
}

void write_text(const char* path, const char* text)
{
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(path)),
                                crd::containers::StringView(text, std::strlen(text))));
}

// two NTSC clips with a dissolve + an audio clip; the SECOND clip has NO source_range — the handler must
// resolve it from the media's available_range
const char* k_cook_otio = R"({
  "OTIO_SCHEMA": "Timeline.1", "name": "cooked-timeline",
  "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
    {"OTIO_SCHEMA": "Track.1", "kind": "Video", "name": "V1", "children": [
      {"OTIO_SCHEMA": "Clip.2", "name": "shotA", "active_media_reference_key": "DEFAULT_MEDIA",
       "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "ExternalReference.1", "target_url": "file:///m/A.mov"}},
       "source_range": {"OTIO_SCHEMA": "TimeRange.1",
         "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 48.0},
         "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 100.0}}},
      {"OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve", "name": "x",
       "in_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 6.0},
       "out_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 6.0}},
      {"OTIO_SCHEMA": "Clip.2", "name": "shotB", "active_media_reference_key": "DEFAULT_MEDIA",
       "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "ExternalReference.1", "target_url": "file:///m/B.mov",
         "available_range": {"OTIO_SCHEMA": "TimeRange.1",
           "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 96.0},
           "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 10.0}}}},
       "source_range": null}
    ]},
    {"OTIO_SCHEMA": "Track.1", "kind": "Audio", "name": "A1", "children": [
      {"OTIO_SCHEMA": "Clip.2", "name": "mix", "active_media_reference_key": "DEFAULT_MEDIA",
       "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "MissingReference.1"}},
       "source_range": {"OTIO_SCHEMA": "TimeRange.1",
         "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 48000.0, "value": 288000.0},
         "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 48000.0, "value": 0.0}}}
    ]}
  ]}
})";

} // namespace

TEST_CASE("cooker: .otio cooks to TIML, loads, and the edit survives exactly", "[cooker][timeline]")
{
    const char* src_path  = "cerid_cook_timeline.otio";
    const char* meta_path = "cerid_cook_timeline.otio.meta";
    write_text(src_path, k_cook_otio);
    write_text(meta_path, "[id]\nuuid = \"aabbccdd00112233aabbccdd00112233\"\n");

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.meta_path   = crd::containers::StringView(meta_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;

    const crd::cooker::CookResult result = otio_handler()(ctx);
    REQUIRE(result.ok);
    CHECK(result.type_fourcc == crd::timeline::kFourCC_TIML);

    // load through the REAL loader
    crd::timeline::TimelineLoader loader(&g_alloc);
    crd::resources::LoadContext   lctx;
    lctx.id        = ctx.id;
    lctx.bytes     = crd::containers::as_const_span(result.cooked_bytes);
    lctx.allocator = &g_alloc;
    auto* tl       = static_cast<crd::timeline::TimelineResource*>(loader.load(lctx));
    REQUIRE(tl != nullptr);

    CHECK(std::strcmp(tl->name(), "cooked-timeline") == 0);
    REQUIRE(tl->tracks.size() == 2);
    REQUIRE(tl->items.size() == 4);

    // NTSC survives as the EXACT rational
    const crd::timeline::ItemRec& shot_a = tl->items[0];
    CHECK(shot_a.source_range.start.value == 100);
    CHECK(shot_a.source_range.start.rate == crd::time::kRateNtsc24);

    // the resolved trim: shotB had NO source_range — the media's available_range landed
    const crd::timeline::ItemRec& shot_b = tl->items[2];
    CHECK(shot_b.has_source_range == 1);
    CHECK(shot_b.source_range.start.value == 10);
    CHECK(shot_b.source_range.duration.value == 96);

    // the edit EVALUATES: track duration = 48 + 96 NTSC frames; the dissolve window is live at the cut
    CHECK(crd::timeline::track_duration(*tl, tl->tracks[0]) ==
          crd::time::RationalTime{144, crd::time::kRateNtsc24});
    crd::containers::Array<crd::timeline::ActiveClip> active(&g_alloc);
    crd::timeline::evaluate_tracks(*tl, crd::time::RationalTime{48, crd::time::kRateNtsc24}, active);
    REQUIRE(active.size() == 3); // outgoing + incoming at the cut, plus audio
    CHECK(active[0].weight == 0.5F);
    CHECK(active[1].weight == 0.5F);

    // audio in SAMPLES
    CHECK(tl->items[3].source_range.duration.value == 288000);
    CHECK(tl->items[3].source_range.duration.rate.num == 48000);

    loader.unload(tl);

    // determinism: a second cook through a FRESH CookIO is byte-identical
    crd::cooker::CookContext ctx2 = ctx;
    crd::cooker::CookIO      ctx_io2(ctx2.source_path, ctx2.meta_path, &g_alloc);
    ctx2.io                       = &ctx_io2;
    const crd::cooker::CookResult again = otio_handler()(ctx2);
    REQUIRE(again.ok);
    REQUIRE(again.cooked_bytes.size() == result.cooked_bytes.size());
    CHECK(std::memcmp(again.cooked_bytes.data(), result.cooked_bytes.data(), result.cooked_bytes.size()) == 0);
}

TEST_CASE("cooker: an untimeable .otio clip refuses with ok=false", "[cooker][timeline]")
{
    const char* bad = R"({
      "OTIO_SCHEMA": "Timeline.1", "name": "bad",
      "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
        {"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
          {"OTIO_SCHEMA": "Clip.2", "name": "noRange", "active_media_reference_key": "DEFAULT_MEDIA",
           "media_references": {"DEFAULT_MEDIA": {"OTIO_SCHEMA": "MissingReference.1"}},
           "source_range": null}
        ]}
      ]}
    })";
    const char* src_path  = "cerid_cook_timeline_bad.otio";
    const char* meta_path = "cerid_cook_timeline_bad.otio.meta";
    write_text(src_path, bad);
    write_text(meta_path, "[id]\nuuid = \"aabbccdd00112233aabbccdd00112244\"\n");

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.meta_path   = crd::containers::StringView(meta_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc);
    ctx.io          = &ctx_io;

    const crd::cooker::CookResult result = otio_handler()(ctx);
    CHECK_FALSE(result.ok);
}
