// otio_export.cpp — GEO-9: the `.otio` WRITER — current schemas (Clip.2 · Marker.2), rational times through the
// %.17g exact f64 edge. import(export(x)) == x is gated; the official OTIO library reads our files (the
// reference-NLE oracle). Keys emit in each schema's canonical alphabetical order (the OTIO lib's own layout) so
// reference diffs stay readable.

#include <crd/assetio/json_write.hpp>
#include <crd/assetio/otio.hpp>

namespace crd::assetio
{

namespace
{
    void write_rational_time(JsonWriter& w, const crd::time::RationalTime& t)
    {
        w.begin_object();
        w.kv("OTIO_SCHEMA", "RationalTime.1");
        w.kv_f64_exact("rate", crd::time::rate_to_f64(t.rate));
        w.kv_f64_exact("value", crd::time::value_to_f64(t));
        w.end_object();
    }

    void write_time_range(JsonWriter& w, const crd::time::TimeRange& r)
    {
        w.begin_object();
        w.kv("OTIO_SCHEMA", "TimeRange.1");
        w.key("duration");
        write_rational_time(w, r.duration);
        w.key("start_time");
        write_rational_time(w, r.start);
        w.end_object();
    }

    void write_available_range(JsonWriter& w, const ImportedMediaRef& ref)
    {
        w.key("available_range");
        if (ref.has_available_range) { write_time_range(w, ref.available_range); }
        else
        {
            w.value_null();
        }
    }

    void write_media_ref(JsonWriter& w, const ImportedMediaRef& ref)
    {
        w.begin_object();
        switch (ref.kind)
        {
        case OtioMediaKind::External:
            w.kv("OTIO_SCHEMA", "ExternalReference.1");
            write_available_range(w, ref);
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", ref.name.c_str());
            w.kv("target_url", ref.url.c_str());
            break;
        case OtioMediaKind::ImageSequence:
            w.kv("OTIO_SCHEMA", "ImageSequenceReference.1");
            write_available_range(w, ref);
            w.kv("frame_step", static_cast<crd::i64>(ref.frame_step));
            w.kv("frame_zero_padding", static_cast<crd::i64>(ref.zero_padding));
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("missing_frame_policy", "error");
            w.kv("name", ref.name.c_str());
            w.kv("name_prefix", ref.name_prefix.c_str());
            w.kv("name_suffix", ref.name_suffix.c_str());
            w.kv_f64_exact("rate", crd::time::rate_to_f64(ref.seq_rate));
            w.kv("start_frame", static_cast<crd::i64>(ref.start_frame));
            w.kv("target_url_base", ref.url.c_str());
            break;
        case OtioMediaKind::Missing:
            w.kv("OTIO_SCHEMA", "MissingReference.1");
            write_available_range(w, ref);
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", ref.name.c_str());
            break;
        }
        w.end_object();
    }

    void write_markers(JsonWriter& w, const ImportedTimeline& tl, crd::u32 first, crd::u32 count)
    {
        w.begin_array();
        for (crd::u32 i = 0; i < count; ++i)
        {
            const ImportedMarker& m = tl.markers[first + i];
            w.begin_object();
            w.kv("OTIO_SCHEMA", "Marker.2");
            w.kv("color", m.color.empty() ? "RED" : m.color.c_str());
            w.key("marked_range");
            write_time_range(w, m.marked_range);
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", m.name.c_str());
            w.end_object();
        }
        w.end_array();
    }

    void write_effects(JsonWriter& w, const ImportedTimeline& tl, crd::u32 first, crd::u32 count)
    {
        w.begin_array();
        for (crd::u32 i = 0; i < count; ++i)
        {
            const ImportedEffect& fx = tl.effects[first + i];
            w.begin_object();
            if (fx.type == OtioEffectType::FreezeFrame)
            {
                w.kv("OTIO_SCHEMA", "FreezeFrame.1");
                w.kv("effect_name", "");
                w.key("metadata");
                w.begin_object();
                w.end_object();
                w.kv("name", "");
                w.kv_f64_exact("time_scalar", 0.0);
            }
            else
            {
                w.kv("OTIO_SCHEMA", "LinearTimeWarp.1");
                w.kv("effect_name", "");
                w.key("metadata");
                w.begin_object();
                w.end_object();
                w.kv("name", "");
                w.kv_f64_exact("time_scalar", fx.time_scalar);
            }
            w.end_object();
        }
        w.end_array();
    }

    void write_item(JsonWriter& w, const ImportedTimeline& tl, const ImportedTimelineItem& item)
    {
        w.begin_object();
        switch (item.type)
        {
        case OtioItemType::Clip:
            w.kv("OTIO_SCHEMA", "Clip.2");
            w.kv("active_media_reference_key", "DEFAULT_MEDIA");
            w.key("effects");
            write_effects(w, tl, item.first_effect, item.effect_count);
            w.key("markers");
            write_markers(w, tl, item.first_marker, item.marker_count);
            w.key("media_references");
            w.begin_object();
            w.key("DEFAULT_MEDIA");
            if (item.media_ref != kOtioInvalid) { write_media_ref(w, tl.media[item.media_ref]); }
            else
            {
                w.begin_object();
                w.kv("OTIO_SCHEMA", "MissingReference.1");
                w.end_object();
            }
            w.end_object();
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", item.name.c_str());
            w.key("source_range");
            if (item.has_source_range) { write_time_range(w, item.source_range); }
            else
            {
                w.value_null();
            }
            break;
        case OtioItemType::Gap:
            w.kv("OTIO_SCHEMA", "Gap.1");
            w.key("effects");
            write_effects(w, tl, item.first_effect, item.effect_count);
            w.key("markers");
            write_markers(w, tl, item.first_marker, item.marker_count);
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", item.name.c_str());
            w.key("source_range");
            write_time_range(w, item.source_range);
            break;
        case OtioItemType::Transition:
            w.kv("OTIO_SCHEMA", "Transition.1");
            w.key("in_offset");
            write_rational_time(w, item.in_offset);
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", item.name.c_str());
            w.key("out_offset");
            write_rational_time(w, item.out_offset);
            w.kv("transition_type",
                 item.transition_type.empty() ? "SMPTE_Dissolve" : item.transition_type.c_str());
            break;
        }
        w.end_object();
    }
} // namespace

crd::containers::String otio_export(const ImportedTimeline& timeline, crd::memory::IAllocator* alloc)
{
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("OTIO_SCHEMA", "Timeline.1");
    w.key("global_start_time");
    if (timeline.has_global_start) { write_rational_time(w, timeline.global_start); }
    else
    {
        w.value_null();
    }
    w.key("metadata");
    w.begin_object();
    w.end_object();
    w.kv("name", timeline.name.c_str());
    w.key("tracks");
    {
        w.begin_object();
        w.kv("OTIO_SCHEMA", "Stack.1");
        w.key("children");
        w.begin_array();
        for (crd::usize t = 0; t < timeline.tracks.size(); ++t)
        {
            const ImportedTrack& track = timeline.tracks[t];
            w.begin_object();
            w.kv("OTIO_SCHEMA", "Track.1");
            w.key("children");
            w.begin_array();
            for (crd::u32 i = 0; i < track.item_count; ++i)
            {
                write_item(w, timeline, timeline.items[track.first_item + i]);
            }
            w.end_array();
            w.key("effects");
            w.begin_array();
            w.end_array();
            const char* kind = track.kind_name.c_str();
            if (track.kind == OtioTrackKind::Video) { kind = "Video"; }
            else if (track.kind == OtioTrackKind::Audio) { kind = "Audio"; }
            w.kv("kind", kind);
            w.key("markers");
            write_markers(w, timeline, track.first_marker, track.marker_count);
            w.key("metadata");
            w.begin_object();
            w.end_object();
            w.kv("name", track.name.c_str());
            w.key("source_range");
            w.value_null();
            w.end_object();
        }
        w.end_array();
        w.key("effects");
        w.begin_array();
        w.end_array();
        w.key("markers");
        write_markers(w, timeline, timeline.first_marker, timeline.marker_count);
        w.key("metadata");
        w.begin_object();
        w.end_object();
        w.kv("name", "tracks");
        w.key("source_range");
        w.value_null();
        w.end_object();
    }
    w.end_object();
    return crd::containers::String(w.str()); // copy out (the writer owns its buffer)
}

} // namespace crd::assetio
