// test_otio.cpp — GEO-9 (D-007 row 74): the `.otio` interchange gates.
//  · a spec-shaped NLE document imports with EXACT rational times (NTSC 23.976 snaps to 24000/1001; frame
//    values are integers, never floats-that-drift)
//  · every schema on the supported surface parses: Clip.2 (active key) + Clip.1 (legacy), External/Missing/
//    ImageSequence references, Gap, Transition, Marker.2 at item/track/stack level, LinearTimeWarp/FreezeFrame
//  · degradations COUNT (GeneratorReference → Missing; an unknown effect skips); refusals NAME the schema
//    (nested Stack); malformed content refuses with the reason
//  · the ROUND TRIP: export(import(x)) re-imports STRUCTURALLY EQUAL — times exact, names verbatim, weights
//    of every table identical (the reference-NLE oracle rides the cooked fixture in tests/cooker)

#include <crd/assetio/otio.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(64U << 20U);
    return a;
}

containers::ConstSpan<u8> bytes_of(const char* s)
{
    return {reinterpret_cast<const u8*>(s), std::strlen(s)};
}

// the reference document: 23.976 NTSC, two video clips with a centered dissolve, an audio clip, an image
// sequence master, a legacy Clip.1, markers at every level, a warp, a freeze, one unknown effect, one
// generator reference (degrades), a gap
const char* k_reference_otio = R"({
  "OTIO_SCHEMA": "Timeline.1",
  "global_start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 86400.0},
  "metadata": {"app": {"vendor": "somewhere"}},
  "name": "geo9-reference",
  "tracks": {
    "OTIO_SCHEMA": "Stack.1",
    "children": [
      {
        "OTIO_SCHEMA": "Track.1",
        "kind": "Video",
        "name": "V1",
        "children": [
          {
            "OTIO_SCHEMA": "Clip.2",
            "name": "takeA",
            "active_media_reference_key": "DEFAULT_MEDIA",
            "media_references": {
              "DEFAULT_MEDIA": {
                "OTIO_SCHEMA": "ExternalReference.1",
                "name": "takeA_media",
                "target_url": "file:///takes/A.mov",
                "available_range": {
                  "OTIO_SCHEMA": "TimeRange.1",
                  "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 240.0},
                  "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 0.0}
                }
              },
              "proxy": {"OTIO_SCHEMA": "ExternalReference.1", "target_url": "file:///takes/A_proxy.mov"}
            },
            "source_range": {
              "OTIO_SCHEMA": "TimeRange.1",
              "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 48.0},
              "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 100.0}
            },
            "markers": [
              {
                "OTIO_SCHEMA": "Marker.2",
                "name": "sync",
                "color": "RED",
                "marked_range": {
                  "OTIO_SCHEMA": "TimeRange.1",
                  "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 1.0},
                  "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 10.0}
                }
              }
            ],
            "effects": [
              {"OTIO_SCHEMA": "LinearTimeWarp.1", "effect_name": "", "name": "slow", "time_scalar": 0.5},
              {"OTIO_SCHEMA": "Blur.1", "effect_name": "blur", "radius": 4.0}
            ]
          },
          {
            "OTIO_SCHEMA": "Transition.1",
            "name": "cross",
            "transition_type": "SMPTE_Dissolve",
            "in_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 12.0},
            "out_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 12.0}
          },
          {
            "OTIO_SCHEMA": "Clip.2",
            "name": "takeB",
            "active_media_reference_key": "DEFAULT_MEDIA",
            "media_references": {
              "DEFAULT_MEDIA": {
                "OTIO_SCHEMA": "ImageSequenceReference.1",
                "name": "takeB_seq",
                "target_url_base": "file:///renders/shotB/",
                "name_prefix": "shotB.",
                "name_suffix": ".exr",
                "start_frame": 1001,
                "frame_step": 1,
                "frame_zero_padding": 4,
                "rate": 23.976023976023978
              }
            },
            "source_range": {
              "OTIO_SCHEMA": "TimeRange.1",
              "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 60.0},
              "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 0.0}
            },
            "effects": [{"OTIO_SCHEMA": "FreezeFrame.1", "effect_name": "", "name": ""}]
          },
          {
            "OTIO_SCHEMA": "Gap.1",
            "name": "",
            "source_range": {
              "OTIO_SCHEMA": "TimeRange.1",
              "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 24.0},
              "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 0.0}
            }
          },
          {
            "OTIO_SCHEMA": "Clip.1",
            "name": "legacyTail",
            "media_reference": {"OTIO_SCHEMA": "GeneratorReference.1", "generator_kind": "SMPTEBars"},
            "source_range": {
              "OTIO_SCHEMA": "TimeRange.1",
              "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 12.0},
              "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 0.0}
            }
          }
        ],
        "markers": [
          {
            "OTIO_SCHEMA": "Marker.2",
            "name": "trackNote",
            "color": "PURPLE",
            "marked_range": {
              "OTIO_SCHEMA": "TimeRange.1",
              "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 2.0},
              "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 23.976023976023978, "value": 50.0}
            }
          }
        ]
      },
      {
        "OTIO_SCHEMA": "Track.1",
        "kind": "Audio",
        "name": "A1",
        "children": [
          {
            "OTIO_SCHEMA": "Clip.2",
            "name": "music",
            "active_media_reference_key": "DEFAULT_MEDIA",
            "media_references": {
              "DEFAULT_MEDIA": {"OTIO_SCHEMA": "MissingReference.1", "name": "tbd"}
            },
            "source_range": {
              "OTIO_SCHEMA": "TimeRange.1",
              "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 48000.0, "value": 192000.0},
              "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 48000.0, "value": 0.0}
            }
          }
        ]
      }
    ],
    "markers": [
      {
        "OTIO_SCHEMA": "Marker.2",
        "name": "stackNote",
        "color": "GREEN",
        "marked_range": {
          "OTIO_SCHEMA": "TimeRange.1",
          "duration": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 96.0},
          "start_time": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 0.0}
        }
      }
    ],
    "metadata": {},
    "name": "tracks"
  }
})";

constexpr time::RationalRate kNtsc = time::kRateNtsc24;

// structural equality of two imported timelines — every table, every time EXACT
void check_equal(const assetio::ImportedTimeline& a, const assetio::ImportedTimeline& b)
{
    CHECK(std::strcmp(a.name.c_str(), b.name.c_str()) == 0);
    CHECK(a.has_global_start == b.has_global_start);
    if (a.has_global_start) { CHECK(time::compare(a.global_start, b.global_start) == 0); }

    REQUIRE(a.tracks.size() == b.tracks.size());
    REQUIRE(a.items.size() == b.items.size());
    REQUIRE(a.media.size() == b.media.size());
    REQUIRE(a.effects.size() == b.effects.size());
    REQUIRE(a.markers.size() == b.markers.size());

    for (usize i = 0; i < a.tracks.size(); ++i)
    {
        CHECK(a.tracks[i].kind == b.tracks[i].kind);
        CHECK(std::strcmp(a.tracks[i].name.c_str(), b.tracks[i].name.c_str()) == 0);
        CHECK(a.tracks[i].first_item == b.tracks[i].first_item);
        CHECK(a.tracks[i].item_count == b.tracks[i].item_count);
        CHECK(a.tracks[i].marker_count == b.tracks[i].marker_count);
    }
    for (usize i = 0; i < a.items.size(); ++i)
    {
        CHECK(a.items[i].type == b.items[i].type);
        CHECK(std::strcmp(a.items[i].name.c_str(), b.items[i].name.c_str()) == 0);
        CHECK(a.items[i].has_source_range == b.items[i].has_source_range);
        if (a.items[i].has_source_range)
        {
            CHECK(time::compare(a.items[i].source_range.start, b.items[i].source_range.start) == 0);
            CHECK(time::compare(a.items[i].source_range.duration, b.items[i].source_range.duration) == 0);
        }
        if (a.items[i].type == assetio::OtioItemType::Transition)
        {
            CHECK(time::compare(a.items[i].in_offset, b.items[i].in_offset) == 0);
            CHECK(time::compare(a.items[i].out_offset, b.items[i].out_offset) == 0);
            CHECK(std::strcmp(a.items[i].transition_type.c_str(), b.items[i].transition_type.c_str()) == 0);
        }
        CHECK(a.items[i].effect_count == b.items[i].effect_count);
        CHECK(a.items[i].marker_count == b.items[i].marker_count);
    }
    for (usize i = 0; i < a.media.size(); ++i)
    {
        CHECK(a.media[i].kind == b.media[i].kind);
        CHECK(std::strcmp(a.media[i].url.c_str(), b.media[i].url.c_str()) == 0);
        CHECK(a.media[i].has_available_range == b.media[i].has_available_range);
        CHECK(a.media[i].start_frame == b.media[i].start_frame);
        CHECK(a.media[i].zero_padding == b.media[i].zero_padding);
    }
    for (usize i = 0; i < a.effects.size(); ++i)
    {
        CHECK(a.effects[i].type == b.effects[i].type);
        CHECK(a.effects[i].time_scalar == b.effects[i].time_scalar);
    }
    for (usize i = 0; i < a.markers.size(); ++i)
    {
        CHECK(std::strcmp(a.markers[i].name.c_str(), b.markers[i].name.c_str()) == 0);
        CHECK(std::strcmp(a.markers[i].color.c_str(), b.markers[i].color.c_str()) == 0);
        CHECK(time::compare(a.markers[i].marked_range.start, b.markers[i].marked_range.start) == 0);
    }
}

} // namespace

TEST_CASE("otio import: the reference NLE document, times EXACT", "[otio]")
{
    assetio::ImportedTimeline tl(&galloc());
    assetio::OtioDiag         diag;
    REQUIRE(assetio::otio_parse(bytes_of(k_reference_otio), tl, &diag) == assetio::OtioResult::Ok);

    CHECK(std::strcmp(tl.name.c_str(), "geo9-reference") == 0);

    // 23.976 SNAPPED to the exact rational — the drift doctrine's import edge
    REQUIRE(tl.has_global_start);
    CHECK(tl.global_start.rate == kNtsc);
    CHECK(tl.global_start.value == 86400);

    REQUIRE(tl.tracks.size() == 2);
    CHECK(tl.tracks[0].kind == assetio::OtioTrackKind::Video);
    CHECK(tl.tracks[1].kind == assetio::OtioTrackKind::Audio);
    REQUIRE(tl.tracks[0].item_count == 5); // takeA · transition · takeB · gap · legacyTail
    REQUIRE(tl.tracks[1].item_count == 1);

    const assetio::ImportedTimelineItem& take_a = tl.items[0];
    CHECK(take_a.type == assetio::OtioItemType::Clip);
    CHECK(take_a.source_range.start.value == 100);
    CHECK(take_a.source_range.start.rate == kNtsc);
    CHECK(take_a.source_range.duration.value == 48);
    REQUIRE(take_a.media_ref != assetio::kOtioInvalid);
    CHECK(tl.media[take_a.media_ref].kind == assetio::OtioMediaKind::External);
    CHECK(std::strcmp(tl.media[take_a.media_ref].url.c_str(), "file:///takes/A.mov") == 0);
    CHECK(tl.media[take_a.media_ref].has_available_range);
    CHECK(tl.media[take_a.media_ref].available_range.duration.value == 240);
    CHECK(take_a.marker_count == 1);
    CHECK(take_a.effect_count == 1); // the warp; Blur.1 SKIPPED and counted
    CHECK(tl.effects[take_a.first_effect].type == assetio::OtioEffectType::LinearTimeWarp);
    CHECK(tl.effects[take_a.first_effect].time_scalar == 0.5);

    const assetio::ImportedTimelineItem& cross = tl.items[1];
    CHECK(cross.type == assetio::OtioItemType::Transition);
    CHECK(cross.in_offset.value == 12);
    CHECK(cross.in_offset.rate == kNtsc);
    CHECK(std::strcmp(cross.transition_type.c_str(), "SMPTE_Dissolve") == 0);

    const assetio::ImportedTimelineItem& take_b = tl.items[2];
    REQUIRE(take_b.media_ref != assetio::kOtioInvalid);
    const assetio::ImportedMediaRef& seq = tl.media[take_b.media_ref];
    CHECK(seq.kind == assetio::OtioMediaKind::ImageSequence);
    CHECK(std::strcmp(seq.url.c_str(), "file:///renders/shotB/") == 0);
    CHECK(std::strcmp(seq.name_prefix.c_str(), "shotB.") == 0);
    CHECK(std::strcmp(seq.name_suffix.c_str(), ".exr") == 0);
    CHECK(seq.start_frame == 1001);
    CHECK(seq.zero_padding == 4);
    CHECK(seq.seq_rate == kNtsc);
    CHECK(take_b.effect_count == 1);
    CHECK(tl.effects[take_b.first_effect].type == assetio::OtioEffectType::FreezeFrame);

    CHECK(tl.items[3].type == assetio::OtioItemType::Gap);
    CHECK(tl.items[3].source_range.duration.value == 24);

    // Clip.1 legacy + GeneratorReference degraded to Missing (counted)
    const assetio::ImportedTimelineItem& legacy = tl.items[4];
    CHECK(legacy.type == assetio::OtioItemType::Clip);
    REQUIRE(legacy.media_ref != assetio::kOtioInvalid);
    CHECK(tl.media[legacy.media_ref].kind == assetio::OtioMediaKind::Missing);

    // audio in SAMPLES — exact
    const assetio::ImportedTimelineItem& music = tl.items[5];
    CHECK(music.source_range.duration.value == 192000);
    CHECK(music.source_range.duration.rate == time::make_rate(48000, 1));

    // markers at every level
    CHECK(tl.tracks[0].marker_count == 1);
    CHECK(std::strcmp(tl.markers[tl.tracks[0].first_marker].name.c_str(), "trackNote") == 0);
    CHECK(tl.marker_count == 1);
    CHECK(std::strcmp(tl.markers[tl.first_marker].name.c_str(), "stackNote") == 0);

    // degradations COUNTED, never silent
    CHECK(diag.skipped_effects == 1);     // Blur.1
    CHECK(diag.degraded_media_refs == 1); // GeneratorReference
    CHECK(diag.inactive_media_refs == 1); // takeA's proxy
}

TEST_CASE("otio round trip: export(import(x)) re-imports structurally equal", "[otio]")
{
    assetio::ImportedTimeline first(&galloc());
    REQUIRE(assetio::otio_parse(bytes_of(k_reference_otio), first, nullptr) == assetio::OtioResult::Ok);

    const containers::String  exported = assetio::otio_export(first, &galloc());
    REQUIRE(exported.size() > 0);

    // the reference-NLE oracle hook (the CRD_3MF_DUMP precedent): CRD_OTIO_DUMP=<path> writes our export so
    // the OFFICIAL OpenTimelineIO library can be run over it out-of-process (recorded in the slice close)
    if (const char* dump = std::getenv("CRD_OTIO_DUMP"); dump != nullptr)
    {
        std::FILE* f = std::fopen(dump, "wb");
        REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(exported.c_str(), 1, exported.size(), f) == exported.size());
        (void)std::fclose(f);
    }

    assetio::ImportedTimeline second(&galloc());
    assetio::OtioDiag         diag;
    REQUIRE(assetio::otio_parse({reinterpret_cast<const u8*>(exported.c_str()), exported.size()}, second,
                                &diag) == assetio::OtioResult::Ok);
    CHECK(diag.skipped_effects == 0);     // our own export contains nothing we cannot read back
    CHECK(diag.degraded_media_refs == 0);

    check_equal(first, second);

    // and the second generation is a FIXED POINT: export(import(export(import(x)))) byte-identical
    const containers::String exported2 = assetio::otio_export(second, &galloc());
    REQUIRE(exported2.size() == exported.size());
    CHECK(std::memcmp(exported2.c_str(), exported.c_str(), exported.size()) == 0);
}

TEST_CASE("otio reference-NLE fixture: the OFFICIAL library's own output imports exactly", "[otio]")
{
    // reference_nle.otio was WRITTEN by OpenTimelineIO 0.18.1 (tests/asset-io/data/otio/; generator recorded
    // in the GEO-9 close) — the hermetic half of the oracle: their serializer, our importer.
    const char* data_dir = std::getenv("CRD_ASSETIO_DATA");
    REQUIRE(data_dir != nullptr);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/otio/reference_nle.otio", data_dir);
    std::FILE* f = std::fopen(path, "rb");
    REQUIRE(f != nullptr);
    (void)std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    (void)std::fseek(f, 0, SEEK_SET);
    REQUIRE(size > 0);
    containers::Array<u8> bytes(&galloc());
    bytes.resize(static_cast<usize>(size));
    REQUIRE(std::fread(bytes.data(), 1, static_cast<usize>(size), f) == static_cast<usize>(size));
    (void)std::fclose(f);

    assetio::ImportedTimeline tl(&galloc());
    assetio::OtioDiag         diag;
    REQUIRE(assetio::otio_parse(containers::as_const_span(bytes), tl, &diag) == assetio::OtioResult::Ok);
    CHECK(diag.skipped_effects == 0);
    CHECK(diag.degraded_media_refs == 0);

    CHECK(std::strcmp(tl.name.c_str(), "reference-nle") == 0);
    REQUIRE(tl.has_global_start);
    CHECK(tl.global_start.value == 86400);
    CHECK(tl.global_start.rate == time::kRate24);

    REQUIRE(tl.tracks.size() == 2);
    REQUIRE(tl.tracks[0].item_count == 4); // shotA · dissolve · shotB · gap
    REQUIRE(tl.tracks[1].item_count == 1);

    // their NTSC f64 snaps to OUR exact 24000/1001
    const assetio::ImportedTimelineItem& shot_a = tl.items[0];
    CHECK(shot_a.source_range.start.value == 86);
    CHECK(shot_a.source_range.start.rate == kNtsc);
    CHECK(shot_a.source_range.duration.value == 72);
    CHECK(shot_a.marker_count == 1);
    CHECK(std::strcmp(tl.markers[shot_a.first_marker].name.c_str(), "vfx-start") == 0);
    CHECK(std::strcmp(tl.markers[shot_a.first_marker].color.c_str(), "ORANGE") == 0);
    REQUIRE(shot_a.media_ref != assetio::kOtioInvalid);
    CHECK(tl.media[shot_a.media_ref].kind == assetio::OtioMediaKind::External);
    CHECK(tl.media[shot_a.media_ref].available_range.duration.value == 500);

    CHECK(tl.items[1].type == assetio::OtioItemType::Transition);
    CHECK(tl.items[1].in_offset.value == 6);
    CHECK(tl.items[1].in_offset.rate == kNtsc);

    const assetio::ImportedTimelineItem& shot_b = tl.items[2];
    REQUIRE(shot_b.media_ref != assetio::kOtioInvalid);
    CHECK(tl.media[shot_b.media_ref].kind == assetio::OtioMediaKind::ImageSequence);
    CHECK(tl.media[shot_b.media_ref].start_frame == 1001);
    CHECK(shot_b.effect_count == 1);
    CHECK(tl.effects[shot_b.first_effect].time_scalar == 2.0);

    CHECK(tl.items[3].type == assetio::OtioItemType::Gap);

    const assetio::ImportedTimelineItem& score = tl.items[4];
    CHECK(score.source_range.duration.value == 240000);
    CHECK(score.source_range.duration.rate == time::make_rate(48000, 1));
    CHECK(tl.media[score.media_ref].kind == assetio::OtioMediaKind::Missing);

    // and OUR export of THEIR timeline re-imports equal (the full circle)
    const containers::String  exported = assetio::otio_export(tl, &galloc());
    assetio::ImportedTimeline second(&galloc());
    REQUIRE(assetio::otio_parse({reinterpret_cast<const u8*>(exported.c_str()), exported.size()}, second,
                                nullptr) == assetio::OtioResult::Ok);
    check_equal(tl, second);
}

TEST_CASE("otio refusals name themselves; degradations count", "[otio]")
{
    { // malformed JSON
        assetio::ImportedTimeline tl(&galloc());
        assetio::OtioDiag         diag;
        CHECK(assetio::otio_parse(bytes_of("{\"OTIO_SCHEMA\": \"Timeline.1\","), tl, &diag) ==
              assetio::OtioResult::MalformedJson);
    }
    { // not a timeline
        assetio::ImportedTimeline tl(&galloc());
        assetio::OtioDiag         diag;
        CHECK(assetio::otio_parse(bytes_of("{\"OTIO_SCHEMA\": \"Clip.2\"}"), tl, &diag) ==
              assetio::OtioResult::UnsupportedSchema);
        CHECK(std::strstr(diag.detail, "Clip.2") != nullptr);
    }
    { // a NESTED Stack (compound clip) refuses WITH THE NAME
        const char* nested = R"({
          "OTIO_SCHEMA": "Timeline.1", "name": "x",
          "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
            {"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
              {"OTIO_SCHEMA": "Stack.1", "children": []}
            ]}
          ]}
        })";
        assetio::ImportedTimeline tl(&galloc());
        assetio::OtioDiag         diag;
        CHECK(assetio::otio_parse(bytes_of(nested), tl, &diag) == assetio::OtioResult::UnsupportedSchema);
        CHECK(std::strstr(diag.detail, "Stack.1") != nullptr);
    }
    { // a gap without a source_range is contradictory
        const char* bad_gap = R"({
          "OTIO_SCHEMA": "Timeline.1", "name": "x",
          "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
            {"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [{"OTIO_SCHEMA": "Gap.1", "name": ""}]}
          ]}
        })";
        assetio::ImportedTimeline tl(&galloc());
        assetio::OtioDiag         diag;
        CHECK(assetio::otio_parse(bytes_of(bad_gap), tl, &diag) == assetio::OtioResult::MalformedTimeline);
    }
    { // a transition with a negative offset is contradictory
        const char* bad_tr = R"({
          "OTIO_SCHEMA": "Timeline.1", "name": "x",
          "tracks": {"OTIO_SCHEMA": "Stack.1", "children": [
            {"OTIO_SCHEMA": "Track.1", "kind": "Video", "children": [
              {"OTIO_SCHEMA": "Transition.1", "transition_type": "SMPTE_Dissolve",
               "in_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": -1.0},
               "out_offset": {"OTIO_SCHEMA": "RationalTime.1", "rate": 24.0, "value": 1.0}}
            ]}
          ]}
        })";
        assetio::ImportedTimeline tl(&galloc());
        assetio::OtioDiag         diag;
        CHECK(assetio::otio_parse(bytes_of(bad_tr), tl, &diag) == assetio::OtioResult::MalformedTimeline);
    }
    { // an empty timeline (no tracks Stack) is contradictory
        assetio::ImportedTimeline tl(&galloc());
        assetio::OtioDiag         diag;
        CHECK(assetio::otio_parse(bytes_of("{\"OTIO_SCHEMA\": \"Timeline.1\", \"name\": \"x\"}"), tl, &diag) ==
              assetio::OtioResult::MalformedTimeline);
    }
}
