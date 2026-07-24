// timeline_otio.cpp — GEO-9 (D-007 row 74): the `.otio` cook handler — parse (crd-asset-io) → translate →
// validate → 'TIML' artifact (crd-timeline's builder). The translation is the honest edge work:
//   - clip trims RESOLVE here (a clip without a source_range takes its media's available_range — the artifact
//     is always eval-ready; a clip with NEITHER refuses: an untimeable clip is a contradiction);
//   - media stay URL-referenced (MediaKind::Resource mapping to cooked engine media lands with its consumers —
//     the FORMAT carries the slot from day one);
//   - marker/effect tables copy through index-stable (ImportedTimeline and TimelineResource share layout).
// Deterministic: same source bytes → byte-identical artifact (the GEO-6 incremental gates ride on this).

#include <crd/assetio/otio.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/timeline/timeline_resource.hpp>

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kOtioHandlerVersion = 1U;

[[nodiscard]] crd::timeline::MediaKind media_kind_of(crd::assetio::OtioMediaKind k)
{
    switch (k)
    {
    case crd::assetio::OtioMediaKind::External: return crd::timeline::MediaKind::External;
    case crd::assetio::OtioMediaKind::ImageSequence: return crd::timeline::MediaKind::ImageSequence;
    case crd::assetio::OtioMediaKind::Missing:
    default: return crd::timeline::MediaKind::Missing;
    }
}

// ImportedTimeline → TimelineResource (returns false on the untimeable-clip contradiction)
[[nodiscard]] bool translate(const crd::assetio::ImportedTimeline& in, crd::timeline::TimelineResource& out)
{
    out.name_off         = out.intern(in.name.c_str());
    out.has_global_start = in.has_global_start ? 1 : 0;
    out.global_start     = in.global_start;

    for (const crd::assetio::ImportedMediaRef& m : in.media)
    {
        crd::timeline::MediaRec rec;
        rec.kind                = static_cast<crd::u8>(media_kind_of(m.kind));
        rec.name_off            = out.intern(m.name.c_str());
        rec.url_off             = out.intern(m.url.c_str());
        rec.prefix_off          = out.intern(m.name_prefix.c_str());
        rec.suffix_off          = out.intern(m.name_suffix.c_str());
        rec.has_available_range = m.has_available_range ? 1 : 0;
        rec.available_range     = m.available_range;
        rec.start_frame         = m.start_frame;
        rec.frame_step          = m.frame_step;
        rec.zero_padding        = m.zero_padding;
        rec.seq_rate            = m.seq_rate;
        out.media.push_back(rec);
    }

    for (const crd::assetio::ImportedMarker& m : in.markers)
    {
        crd::timeline::MarkerRec rec;
        rec.name_off  = out.intern(m.name.c_str());
        rec.color_off = out.intern(m.color.c_str());
        rec.range     = m.marked_range;
        out.markers.push_back(rec);
    }
    out.first_marker = in.first_marker;
    out.marker_count = in.marker_count;

    for (const crd::assetio::ImportedEffect& e : in.effects)
    {
        crd::timeline::EffectRec rec;
        rec.type = static_cast<crd::u8>(e.type == crd::assetio::OtioEffectType::FreezeFrame
                                            ? crd::timeline::EffectType::FreezeFrame
                                            : crd::timeline::EffectType::LinearTimeWarp);
        rec.time_scalar = e.time_scalar;
        out.effects.push_back(rec);
    }

    for (const crd::assetio::ImportedTrack& t : in.tracks)
    {
        crd::timeline::TrackRec rec;
        crd::timeline::TrackKind kind = crd::timeline::TrackKind::Other;
        if (t.kind == crd::assetio::OtioTrackKind::Video) { kind = crd::timeline::TrackKind::Video; }
        else if (t.kind == crd::assetio::OtioTrackKind::Audio) { kind = crd::timeline::TrackKind::Audio; }
        rec.kind = static_cast<crd::u8>(kind);
        rec.kind_name_off = out.intern(t.kind_name.c_str());
        rec.name_off      = out.intern(t.name.c_str());
        rec.first_item    = t.first_item;
        rec.item_count    = t.item_count;
        rec.first_marker  = t.first_marker;
        rec.marker_count  = t.marker_count;
        out.tracks.push_back(rec);
    }

    for (const crd::assetio::ImportedTimelineItem& item : in.items)
    {
        crd::timeline::ItemRec rec;
        crd::timeline::ItemType type = crd::timeline::ItemType::Transition;
        if (item.type == crd::assetio::OtioItemType::Clip) { type = crd::timeline::ItemType::Clip; }
        else if (item.type == crd::assetio::OtioItemType::Gap) { type = crd::timeline::ItemType::Gap; }
        rec.type = static_cast<crd::u8>(type);
        rec.name_off            = out.intern(item.name.c_str());
        rec.media_ref           = item.media_ref; // index-stable (kOtioInvalid == kInvalidIndex)
        rec.transition_type_off = out.intern(item.transition_type.c_str());
        rec.in_offset           = item.in_offset;
        rec.out_offset          = item.out_offset;
        rec.first_effect        = item.first_effect;
        rec.effect_count        = item.effect_count;
        rec.first_marker        = item.first_marker;
        rec.marker_count        = item.marker_count;

        if (item.has_source_range)
        {
            rec.has_source_range = 1;
            rec.source_range     = item.source_range;
        }
        else if (item.type == crd::assetio::OtioItemType::Clip)
        {
            // RESOLVE the trim from the media's available range — the artifact is always eval-ready
            if (item.media_ref == crd::assetio::kOtioInvalid ||
                !in.media[item.media_ref].has_available_range)
            {
                return false; // an untimeable clip is a contradiction — the processor reports the failed cook
            }
            rec.has_source_range = 1;
            rec.source_range     = in.media[item.media_ref].available_range;
        }
        out.items.push_back(rec);
    }
    return true;
}

CookResult otio_timeline_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);
    result.type_fourcc     = crd::timeline::kFourCC_TIML;
    result.handler_version = kOtioHandlerVersion;
    if (ctx.io == nullptr) { return result; }

    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!ctx.io->read_source(src)) { return result; }

    crd::assetio::ImportedTimeline imported(ctx.allocator);
    crd::assetio::OtioDiag         diag;
    if (crd::assetio::otio_parse(crd::containers::as_const_span(src), imported, &diag) !=
        crd::assetio::OtioResult::Ok)
    {
        return result; // diag names the refused schema; the processor reports the failed cook
    }

    crd::timeline::TimelineResource tl(ctx.allocator);
    if (!translate(imported, tl)) { return result; }

    result.cooked_bytes = crd::timeline::timeline_build(tl, ctx.id, ctx.allocator);
    if (result.cooked_bytes.size() == 0) { return result; } // structural validation refused
    result.ok = true;
    return result;
}

} // namespace

void register_otio_timeline_handler()
{
    register_cook_handler(".otio", otio_timeline_handler, kOtioHandlerVersion);
}

} // namespace crd::cooker
