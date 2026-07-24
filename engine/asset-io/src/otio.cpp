// otio.cpp — GEO-9: the `.otio` READER (see otio.hpp for the fidelity contract). Structure mirrors the OTIO
// object model: Timeline → Stack("tracks") → Track → children (Clip/Gap/Transition), every time value through
// the exact `time_from_f64` edge. Refusals NAME the schema; degradations COUNT in the diag — never silent.

#include <crd/assetio/json.hpp>
#include <crd/assetio/otio.hpp>

#include <cstdio>
#include <cstring>
#include <utility>

namespace crd::assetio
{

namespace
{
    using json::JsonDoc;

    struct Ctx
    {
        const JsonDoc&        doc;
        ImportedTimeline&     out;
        OtioDiag*             diag;
        crd::memory::IAllocator* alloc;
    };

    void set_detail(OtioDiag* diag, const char* what, const char* schema)
    {
        if (diag == nullptr) { return; }
        std::snprintf(diag->detail, sizeof(diag->detail), "%s%s%s", what, schema != nullptr ? ": " : "",
                      schema != nullptr ? schema : "");
    }

    // "OTIO_SCHEMA": "Name.Version" — match `Name` (any version; OTIO versions add fields, never break readers)
    [[nodiscard]] bool schema_is(const JsonDoc& doc, crd::u32 obj, const char* name)
    {
        const crd::u32 s = json::find(doc, obj, "OTIO_SCHEMA");
        if (s == json::kInvalid || doc.nodes[s].type != json::JsonType::String) { return false; }
        const char*      text = doc.strings.data() + doc.nodes[s].str_off;
        const crd::u32   len  = doc.nodes[s].str_len;
        const crd::usize n    = std::strlen(name);
        if (len < n + 2 || std::memcmp(text, name, n) != 0) { return false; }
        return text[n] == '.';
    }

    // copy a schema string into `buf` for diagnostics (never for logic)
    void schema_text(const JsonDoc& doc, crd::u32 obj, char* buf, crd::u32 cap)
    {
        buf[0]           = '\0';
        const crd::u32 s = json::find(doc, obj, "OTIO_SCHEMA");
        if (s != json::kInvalid) { (void)json::str_value(doc, s, buf, cap); }
    }

    void read_string(const JsonDoc& doc, crd::u32 node, crd::containers::String& out)
    {
        out = "";
        if (node == json::kInvalid || doc.nodes[node].type != json::JsonType::String) { return; }
        out.append(doc.strings.data() + doc.nodes[node].str_off, doc.nodes[node].str_len);
    }

    [[nodiscard]] bool is_null(const JsonDoc& doc, crd::u32 node)
    {
        return node == json::kInvalid || doc.nodes[node].type == json::JsonType::Null;
    }

    [[nodiscard]] bool parse_rational_time(const JsonDoc& doc, crd::u32 node, crd::time::RationalTime& out)
    {
        if (node == json::kInvalid || doc.nodes[node].type != json::JsonType::Object) { return false; }
        if (!schema_is(doc, node, "RationalTime")) { return false; }
        const crd::f64 rate  = json::as_f64(doc, json::find(doc, node, "rate"), 0.0);
        const crd::f64 value = json::as_f64(doc, json::find(doc, node, "value"), 0.0);
        out                  = crd::time::time_from_f64(value, rate);
        return out.valid();
    }

    [[nodiscard]] bool parse_time_range(const JsonDoc& doc, crd::u32 node, crd::time::TimeRange& out)
    {
        if (node == json::kInvalid || doc.nodes[node].type != json::JsonType::Object) { return false; }
        if (!schema_is(doc, node, "TimeRange")) { return false; }
        if (!parse_rational_time(doc, json::find(doc, node, "start_time"), out.start)) { return false; }
        if (!parse_rational_time(doc, json::find(doc, node, "duration"), out.duration)) { return false; }
        return out.valid();
    }

    // one media-reference OBJECT → ImportedMediaRef appended; unknown schemas degrade to Missing (counted)
    [[nodiscard]] crd::u32 parse_media_ref(Ctx& ctx, crd::u32 node)
    {
        ImportedMediaRef ref(ctx.alloc);
        if (!is_null(ctx.doc, node) && ctx.doc.nodes[node].type == json::JsonType::Object)
        {
            read_string(ctx.doc, json::find(ctx.doc, node, "name"), ref.name);
            const crd::u32 avail = json::find(ctx.doc, node, "available_range");
            if (!is_null(ctx.doc, avail))
            {
                ref.has_available_range = parse_time_range(ctx.doc, avail, ref.available_range);
            }
            if (schema_is(ctx.doc, node, "ExternalReference"))
            {
                ref.kind = OtioMediaKind::External;
                read_string(ctx.doc, json::find(ctx.doc, node, "target_url"), ref.url);
            }
            else if (schema_is(ctx.doc, node, "ImageSequenceReference"))
            {
                ref.kind = OtioMediaKind::ImageSequence;
                read_string(ctx.doc, json::find(ctx.doc, node, "target_url_base"), ref.url);
                read_string(ctx.doc, json::find(ctx.doc, node, "name_prefix"), ref.name_prefix);
                read_string(ctx.doc, json::find(ctx.doc, node, "name_suffix"), ref.name_suffix);
                ref.start_frame = static_cast<crd::i32>(
                    json::as_i64(ctx.doc, json::find(ctx.doc, node, "start_frame"), 0));
                ref.frame_step = static_cast<crd::i32>(
                    json::as_i64(ctx.doc, json::find(ctx.doc, node, "frame_step"), 1));
                ref.zero_padding = static_cast<crd::i32>(
                    json::as_i64(ctx.doc, json::find(ctx.doc, node, "frame_zero_padding"), 0));
                ref.seq_rate =
                    crd::time::rate_from_f64(json::as_f64(ctx.doc, json::find(ctx.doc, node, "rate"), 0.0));
            }
            else if (schema_is(ctx.doc, node, "MissingReference"))
            {
                ref.kind = OtioMediaKind::Missing;
            }
            else // GeneratorReference et al. — the edit survives, the media is unresolvable here
            {
                ref.kind = OtioMediaKind::Missing;
                if (ctx.diag != nullptr) { ++ctx.diag->degraded_media_refs; }
            }
        }
        ctx.out.media.push_back(std::move(ref));
        return static_cast<crd::u32>(ctx.out.media.size() - 1);
    }

    // an item's `effects` array → contiguous ImportedEffect range (unknown schemas counted + skipped)
    void parse_effects(Ctx& ctx, crd::u32 effects_node, crd::u32& first, crd::u32& count)
    {
        first = kOtioInvalid;
        count = 0;
        if (is_null(ctx.doc, effects_node) || ctx.doc.nodes[effects_node].type != json::JsonType::Array)
        {
            return;
        }
        const crd::u32 n = json::count_of(ctx.doc, effects_node);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u32 e = json::at(ctx.doc, effects_node, i);
            ImportedEffect fx;
            if (schema_is(ctx.doc, e, "FreezeFrame"))
            {
                fx.type        = OtioEffectType::FreezeFrame;
                fx.time_scalar = 0.0;
            }
            else if (schema_is(ctx.doc, e, "LinearTimeWarp"))
            {
                fx.type        = OtioEffectType::LinearTimeWarp;
                fx.time_scalar = json::as_f64(ctx.doc, json::find(ctx.doc, e, "time_scalar"), 1.0);
            }
            else
            {
                if (ctx.diag != nullptr) { ++ctx.diag->skipped_effects; }
                continue;
            }
            if (first == kOtioInvalid) { first = static_cast<crd::u32>(ctx.out.effects.size()); }
            ctx.out.effects.push_back(fx);
            ++count;
        }
    }

    // a `markers` array → contiguous ImportedMarker range (marked_range required — a marker without a place
    // on the timeline is a contradiction)
    [[nodiscard]] bool parse_markers(Ctx& ctx, crd::u32 markers_node, crd::u32& first, crd::u32& count)
    {
        first = kOtioInvalid;
        count = 0;
        if (is_null(ctx.doc, markers_node) || ctx.doc.nodes[markers_node].type != json::JsonType::Array)
        {
            return true;
        }
        const crd::u32 n = json::count_of(ctx.doc, markers_node);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::u32 m = json::at(ctx.doc, markers_node, i);
            if (!schema_is(ctx.doc, m, "Marker")) { continue; } // unknown marker schema: skip, markers are annotations
            ImportedMarker marker(ctx.alloc);
            read_string(ctx.doc, json::find(ctx.doc, m, "name"), marker.name);
            read_string(ctx.doc, json::find(ctx.doc, m, "color"), marker.color);
            if (!parse_time_range(ctx.doc, json::find(ctx.doc, m, "marked_range"), marker.marked_range))
            {
                set_detail(ctx.diag, "marker without a valid marked_range", nullptr);
                return false;
            }
            if (first == kOtioInvalid) { first = static_cast<crd::u32>(ctx.out.markers.size()); }
            ctx.out.markers.push_back(std::move(marker));
            ++count;
        }
        return true;
    }

    [[nodiscard]] OtioResult parse_item(Ctx& ctx, crd::u32 node)
    {
        if (node == json::kInvalid || ctx.doc.nodes[node].type != json::JsonType::Object)
        {
            set_detail(ctx.diag, "track child is not an object", nullptr);
            return OtioResult::MalformedTimeline;
        }
        ImportedTimelineItem item(ctx.alloc);
        read_string(ctx.doc, json::find(ctx.doc, node, "name"), item.name);

        if (schema_is(ctx.doc, node, "Clip"))
        {
            item.type = OtioItemType::Clip;
            // Clip.2: media_references dict + active key; Clip.1: media_reference — the ACTIVE reference imports
            const crd::u32 refs = json::find(ctx.doc, node, "media_references");
            if (refs != json::kInvalid && ctx.doc.nodes[refs].type == json::JsonType::Object)
            {
                char active[128] = "DEFAULT_MEDIA";
                (void)json::str_value(ctx.doc, json::find(ctx.doc, node, "active_media_reference_key"), active,
                                      sizeof(active));
                const crd::u32 chosen = json::find(ctx.doc, refs, active);
                if (chosen == json::kInvalid)
                {
                    set_detail(ctx.diag, "active_media_reference_key names no reference", nullptr);
                    return OtioResult::MalformedTimeline;
                }
                item.media_ref = parse_media_ref(ctx, chosen);
                if (ctx.diag != nullptr && json::count_of(ctx.doc, refs) > 1)
                {
                    ctx.diag->inactive_media_refs += json::count_of(ctx.doc, refs) - 1;
                }
            }
            else
            {
                item.media_ref = parse_media_ref(ctx, json::find(ctx.doc, node, "media_reference"));
            }
            const crd::u32 sr = json::find(ctx.doc, node, "source_range");
            if (!is_null(ctx.doc, sr))
            {
                if (!parse_time_range(ctx.doc, sr, item.source_range))
                {
                    set_detail(ctx.diag, "clip source_range malformed", nullptr);
                    return OtioResult::MalformedTimeline;
                }
                item.has_source_range = true;
            }
        }
        else if (schema_is(ctx.doc, node, "Gap"))
        {
            item.type = OtioItemType::Gap;
            if (!parse_time_range(ctx.doc, json::find(ctx.doc, node, "source_range"), item.source_range))
            {
                set_detail(ctx.diag, "gap without a valid source_range", nullptr);
                return OtioResult::MalformedTimeline;
            }
            item.has_source_range = true;
        }
        else if (schema_is(ctx.doc, node, "Transition"))
        {
            item.type = OtioItemType::Transition;
            read_string(ctx.doc, json::find(ctx.doc, node, "transition_type"), item.transition_type);
            if (!parse_rational_time(ctx.doc, json::find(ctx.doc, node, "in_offset"), item.in_offset) ||
                !parse_rational_time(ctx.doc, json::find(ctx.doc, node, "out_offset"), item.out_offset) ||
                item.in_offset.value < 0 || item.out_offset.value < 0)
            {
                set_detail(ctx.diag, "transition offsets malformed", nullptr);
                return OtioResult::MalformedTimeline;
            }
        }
        else // nested Stack (compound clip) or an unknown composable — refuse WITH THE NAME
        {
            char schema[64];
            schema_text(ctx.doc, node, schema, sizeof(schema));
            set_detail(ctx.diag, "unsupported track child", schema);
            return OtioResult::UnsupportedSchema;
        }

        parse_effects(ctx, json::find(ctx.doc, node, "effects"), item.first_effect, item.effect_count);
        if (!parse_markers(ctx, json::find(ctx.doc, node, "markers"), item.first_marker, item.marker_count))
        {
            return OtioResult::MalformedTimeline;
        }
        ctx.out.items.push_back(std::move(item));
        return OtioResult::Ok;
    }

    [[nodiscard]] OtioResult parse_track(Ctx& ctx, crd::u32 node)
    {
        if (!schema_is(ctx.doc, node, "Track"))
        {
            char schema[64];
            schema_text(ctx.doc, node, schema, sizeof(schema));
            set_detail(ctx.diag, "stack child is not a Track", schema);
            return OtioResult::UnsupportedSchema; // a Stack-in-Stack (compound) lands here too, named
        }
        ImportedTrack track(ctx.alloc);
        read_string(ctx.doc, json::find(ctx.doc, node, "name"), track.name);
        read_string(ctx.doc, json::find(ctx.doc, node, "kind"), track.kind_name);
        track.kind = OtioTrackKind::Other;
        if (std::strcmp(track.kind_name.c_str(), "Video") == 0) { track.kind = OtioTrackKind::Video; }
        else if (std::strcmp(track.kind_name.c_str(), "Audio") == 0) { track.kind = OtioTrackKind::Audio; }

        track.first_item        = static_cast<crd::u32>(ctx.out.items.size());
        const crd::u32 children = json::find(ctx.doc, node, "children");
        const crd::u32 n        = json::count_of(ctx.doc, children);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const OtioResult r = parse_item(ctx, json::at(ctx.doc, children, i));
            if (r != OtioResult::Ok) { return r; }
        }
        track.item_count = static_cast<crd::u32>(ctx.out.items.size()) - track.first_item;

        if (!parse_markers(ctx, json::find(ctx.doc, node, "markers"), track.first_marker, track.marker_count))
        {
            return OtioResult::MalformedTimeline;
        }
        ctx.out.tracks.push_back(std::move(track));
        return OtioResult::Ok;
    }
} // namespace

OtioResult otio_parse(crd::containers::ConstSpan<crd::u8> bytes, ImportedTimeline& out, OtioDiag* diag)
{
    crd::memory::IAllocator* alloc = out.tracks.allocator();
    JsonDoc                  doc(alloc);
    if (!json::parse(bytes, doc))
    {
        if (diag != nullptr)
        {
            diag->error_off = doc.error_off;
            set_detail(diag, "not valid JSON", nullptr);
        }
        return OtioResult::MalformedJson;
    }

    Ctx ctx{doc, out, diag, alloc};
    if (doc.root == json::kInvalid || doc.nodes[doc.root].type != json::JsonType::Object ||
        !schema_is(doc, doc.root, "Timeline"))
    {
        char schema[64];
        schema_text(doc, doc.root, schema, sizeof(schema));
        set_detail(diag, "root is not a Timeline", schema);
        return OtioResult::UnsupportedSchema;
    }
    read_string(doc, json::find(doc, doc.root, "name"), out.name);

    const crd::u32 gst = json::find(doc, doc.root, "global_start_time");
    if (!is_null(doc, gst))
    {
        if (!parse_rational_time(doc, gst, out.global_start))
        {
            set_detail(diag, "global_start_time malformed", nullptr);
            return OtioResult::MalformedTimeline;
        }
        out.has_global_start = true;
    }

    const crd::u32 stack = json::find(doc, doc.root, "tracks");
    if (is_null(doc, stack) || !schema_is(doc, stack, "Stack"))
    {
        set_detail(diag, "timeline has no tracks Stack", nullptr);
        return OtioResult::MalformedTimeline;
    }
    const crd::u32 children = json::find(doc, stack, "children");
    const crd::u32 n        = json::count_of(doc, children);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const OtioResult r = parse_track(ctx, json::at(doc, children, i));
        if (r != OtioResult::Ok) { return r; }
    }
    if (!parse_markers(ctx, json::find(doc, stack, "markers"), out.first_marker, out.marker_count))
    {
        return OtioResult::MalformedTimeline;
    }
    return OtioResult::Ok;
}

} // namespace crd::assetio
