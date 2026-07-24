#pragma once

// otio.hpp — GEO-9 (D-007 row 74): the OpenTimelineIO interchange edge — OUR OWN `.otio` (JSON) reader/writer
// filling `ImportedTimeline`, the neutral model between NLE files and the cook (the exact glTF posture: OTIO is
// what Resolve/Avid/Premiere speak; the ENGINE's timeline is the cooked TIML resource, never the foreign file).
//
// Fidelity contract:
//   - every RationalTime/TimeRange imports through `crd::time::time_from_f64` — EXACT for every f64 the OTIO
//     library can serialize (SMPTE-family snap + binary-fraction fallback; the drift doctrine's import edge);
//   - schema surface: Timeline · Stack (the top track container ONLY) · Track · Clip.1/.2 · Gap · Transition ·
//     ExternalReference · MissingReference · ImageSequenceReference · Marker.2 · LinearTimeWarp · FreezeFrame.
//     NESTED Stacks (compound clips) REFUSE loudly with the schema named — a wrong-looking cut is worse than a
//     refused import. Unknown MEDIA references degrade to Missing (the edit survives; the media is unresolvable
//     here); unknown EFFECTS are counted in the diag and skipped (the editorial norm — never silent);
//   - `metadata` dicts are not preserved (app-specific baggage; our own metadata story is the resource's).
//
// Export writes current schemas (Clip.2 etc.) with %.17g f64s — our export → our import is IDENTITY (gated),
// and the official OTIO library reads our files verbatim (the reference-NLE oracle gate).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/time/rational_time.hpp>

namespace crd::assetio
{

inline constexpr crd::u32 kOtioInvalid = 0xFFFFFFFFU;

enum class OtioTrackKind : crd::u8
{
    Video = 0,
    Audio,
    Other, // preserved by name on the track — exported verbatim
};

enum class OtioItemType : crd::u8
{
    Clip = 0,
    Gap,
    Transition,
};

enum class OtioMediaKind : crd::u8
{
    External = 0,  // target_url
    Missing,       // an edit whose media is not resolvable here — still a valid edit
    ImageSequence, // target_url_base + prefix/suffix/padding + frame numbering (our EXR-sequence masters)
};

enum class OtioEffectType : crd::u8
{
    LinearTimeWarp = 0, // media time scales by time_scalar about the clip's trimmed start
    FreezeFrame,        // time_scalar 0 — the first trimmed frame holds
};

struct ImportedMediaRef
{
    OtioMediaKind           kind = OtioMediaKind::Missing;
    crd::containers::String name;
    crd::containers::String url; // External: target_url · ImageSequence: target_url_base
    bool                    has_available_range = false;
    crd::time::TimeRange    available_range;
    // ImageSequence only:
    crd::containers::String name_prefix;
    crd::containers::String name_suffix;
    crd::i32                start_frame  = 0;
    crd::i32                frame_step   = 1;
    crd::i32                zero_padding = 0;
    crd::time::RationalRate seq_rate;

    explicit ImportedMediaRef(crd::memory::IAllocator* a) : name(a), url(a), name_prefix(a), name_suffix(a) {}
    ImportedMediaRef(const ImportedMediaRef&)            = delete;
    ImportedMediaRef& operator=(const ImportedMediaRef&) = delete;
    ImportedMediaRef(ImportedMediaRef&&)                 = default;
    ImportedMediaRef& operator=(ImportedMediaRef&&)      = default;
};

struct ImportedEffect
{
    OtioEffectType type        = OtioEffectType::LinearTimeWarp;
    crd::f64       time_scalar = 1.0;
};

struct ImportedMarker
{
    crd::containers::String name;
    crd::containers::String color; // OTIO color names ("RED", "GREEN", …) verbatim
    crd::time::TimeRange    marked_range;

    explicit ImportedMarker(crd::memory::IAllocator* a) : name(a), color(a) {}
    ImportedMarker(const ImportedMarker&)            = delete;
    ImportedMarker& operator=(const ImportedMarker&) = delete;
    ImportedMarker(ImportedMarker&&)                 = default;
    ImportedMarker& operator=(ImportedMarker&&)      = default;
};

// One item in a track's child list. Clips/Gaps occupy sequential time (a track has no explicit item starts —
// positions ACCUMULATE, OTIO's model); a Transition sits BETWEEN two items, borrowing in/out offsets from them.
struct ImportedTimelineItem
{
    OtioItemType            type = OtioItemType::Gap;
    crd::containers::String name;
    bool                    has_source_range = false; // Clip: trims media · Gap: its duration
    crd::time::TimeRange    source_range;
    crd::u32                media_ref = kOtioInvalid; // Clip → ImportedTimeline::media index
    // Transition:
    crd::time::RationalTime in_offset;
    crd::time::RationalTime out_offset;
    crd::containers::String transition_type; // "SMPTE_Dissolve" (the standard cross-dissolve) et al., verbatim
    // per-item attachments (contiguous ranges in ImportedTimeline::effects / ::markers):
    crd::u32 first_effect = kOtioInvalid;
    crd::u32 effect_count = 0;
    crd::u32 first_marker = kOtioInvalid;
    crd::u32 marker_count = 0;

    explicit ImportedTimelineItem(crd::memory::IAllocator* a) : name(a), transition_type(a) {}
    ImportedTimelineItem(const ImportedTimelineItem&)            = delete;
    ImportedTimelineItem& operator=(const ImportedTimelineItem&) = delete;
    ImportedTimelineItem(ImportedTimelineItem&&)                 = default;
    ImportedTimelineItem& operator=(ImportedTimelineItem&&)      = default;
};

struct ImportedTrack
{
    OtioTrackKind           kind = OtioTrackKind::Other;
    crd::containers::String kind_name; // the verbatim OTIO kind string (round-trips Other kinds)
    crd::containers::String name;
    crd::u32                first_item = kOtioInvalid; // contiguous range in ImportedTimeline::items
    crd::u32                item_count = 0;
    crd::u32                first_marker = kOtioInvalid;
    crd::u32                marker_count = 0;

    explicit ImportedTrack(crd::memory::IAllocator* a) : kind_name(a), name(a) {}
    ImportedTrack(const ImportedTrack&)            = delete;
    ImportedTrack& operator=(const ImportedTrack&) = delete;
    ImportedTrack(ImportedTrack&&)                 = default;
    ImportedTrack& operator=(ImportedTrack&&)      = default;
};

struct ImportedTimeline
{
    crd::containers::String                          name;
    bool                                             has_global_start = false;
    crd::time::RationalTime                          global_start;
    crd::containers::Array<ImportedTrack>            tracks;
    crd::containers::Array<ImportedTimelineItem>     items;   // grouped per track (first_item/item_count)
    crd::containers::Array<ImportedMediaRef>         media;   // one per clip reference (dedup is the cook's job)
    crd::containers::Array<ImportedEffect>           effects; // grouped per item
    crd::containers::Array<ImportedMarker>           markers; // grouped per item, then per track, then timeline
    crd::u32                                         first_marker = kOtioInvalid; // timeline-level (the top Stack's)
    crd::u32                                         marker_count = 0;

    explicit ImportedTimeline(crd::memory::IAllocator* a)
        : name(a), tracks(a), items(a), media(a), effects(a), markers(a)
    {
    }
    ImportedTimeline(const ImportedTimeline&)            = delete;
    ImportedTimeline& operator=(const ImportedTimeline&) = delete;
    ImportedTimeline(ImportedTimeline&&)                 = default;
    ImportedTimeline& operator=(ImportedTimeline&&)      = default;
};

enum class OtioResult : crd::u8
{
    Ok = 0,
    MalformedJson,     // not RFC-8259 (diag.error_off = the byte)
    UnsupportedSchema, // a schema this edge refuses (nested Stack, an unknown item type) — named in diag.detail
    MalformedTimeline, // schema fine, content contradictory (negative duration, bad rate, missing children)
};

struct OtioDiag
{
    crd::usize error_off              = 0;  // MalformedJson: byte offset
    crd::u32   skipped_effects        = 0;  // unknown effect schemas dropped (never silent)
    crd::u32   degraded_media_refs    = 0;  // unknown media-reference schemas read as Missing
    crd::u32   inactive_media_refs    = 0;  // Clip.2 non-active references not imported (proxy workflows)
    char       detail[128]            = {}; // the refused schema / the contradiction, NUL-terminated
};

// Parse `.otio` bytes into `out` (cleared first). `diag` is optional but recommended — refusals name themselves.
[[nodiscard]] OtioResult otio_parse(crd::containers::ConstSpan<crd::u8> bytes, ImportedTimeline& out,
                                    OtioDiag* diag);

// Serialize to `.otio` JSON (current schemas, %.17g f64 — import(export(x)) == x, gated). Returns the JSON text.
[[nodiscard]] crd::containers::String otio_export(const ImportedTimeline& timeline, crd::memory::IAllocator* alloc);

} // namespace crd::assetio
