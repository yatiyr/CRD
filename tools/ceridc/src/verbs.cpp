// verbs.cpp — GEO-11: the verb implementations (see verbs.hpp — validate COMPLETELY, then act).

#include <crd/ceridc/verbs.hpp>

#include <crd/assetio/gltf.hpp>
#include <crd/assetio/json_write.hpp>
#include <crd/assetio/obj.hpp>
#include <crd/assetio/otio.hpp>
#include <crd/assetio/ply.hpp>
#include <crd/assetio/stl.hpp>
#include <crd/assetio/threemf.hpp>
#include <crd/audio/aiff.hpp>
#include <crd/audio/flac.hpp>
#include <crd/audio/midi.hpp>
#include <crd/audio/wav.hpp>
#include <crd/cooker/cook_command.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/hdr_image.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/zip_archive.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>
#include <crd/time/rational_time.hpp>
#include <crd/timeline/timeline_eval.hpp>
#include <crd/timeline/timeline_render.hpp>
#include <crd/timeline/timeline_resource.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::ceridc
{

namespace
{
    using crd::assetio::JsonWriter;

    [[nodiscard]] bool ends_with(const char* path, const char* suffix)
    {
        const crd::usize n = std::strlen(path);
        const crd::usize m = std::strlen(suffix);
        if (n < m) { return false; }
        for (crd::usize i = 0; i < m; ++i)
        {
            char c = path[n - m + i];
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
            if (c != suffix[i]) { return false; }
        }
        return true;
    }

    [[nodiscard]] bool read_file(const char* path, crd::containers::Array<crd::u8>& out)
    {
        return fs::read_file_binary(fs::Path(crd::containers::StringView(path)), out);
    }

    [[nodiscard]] crd::containers::String fail(crd::memory::IAllocator* alloc, const char* verb,
                                               const char* reason)
    {
        JsonWriter w(alloc);
        w.begin_object();
        w.kv("ok", false);
        w.kv("verb", verb);
        w.kv("reason", reason);
        w.end_object();
        return crd::containers::String(w.str());
    }

    // manifest entries of a PACK (empty on any structural failure)
    struct PackEntry
    {
        crd::resources::ResourceId id;
        crd::u32                   type_fourcc = 0;
        crd::containers::String    name;
        explicit PackEntry(crd::memory::IAllocator* a) : name(a) {}
        PackEntry(PackEntry&&)            = default;
        PackEntry& operator=(PackEntry&&) = default;
    };

    [[nodiscard]] bool read_pack_entries(const char* pack_path, crd::memory::IAllocator* alloc,
                                         crd::containers::Array<PackEntry>& out)
    {
        crd::containers::Array<crd::u8> bytes(alloc);
        if (!read_file(pack_path, bytes)) { return false; }
        crd::resources::CrdrFile file(alloc);
        if (crd::resources::crdr_read(crd::containers::as_const_span(bytes), file, alloc) !=
                crd::resources::CrdrError::Ok ||
            file.type_fourcc != crd::resources::kFourCC_PACK)
        {
            return false;
        }
        const crd::resources::CrdrChunk* mfst = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_MFST);
        if (mfst == nullptr) { return false; }
        const crd::resources::CrdrChunk* strp = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_STRP);
        crd::containers::Array<crd::resources::ManifestEntry> entries(alloc);
        if (!crd::resources::manifest_read_entries(mfst->payload, entries, alloc)) { return false; }
        for (const crd::resources::ManifestEntry& e : entries)
        {
            PackEntry pe(alloc);
            pe.id          = e.id;
            pe.type_fourcc = e.type_fourcc;
            if (strp != nullptr && e.name_strp_idx < strp->payload.size())
            {
                const char* begin = reinterpret_cast<const char*>(strp->payload.data() + e.name_strp_idx);
                const char* limit = reinterpret_cast<const char*>(strp->payload.data() + strp->payload.size());
                const char* end   = begin;
                while (end < limit && *end != '\0') { ++end; }
                pe.name.append(begin, static_cast<crd::usize>(end - begin));
            }
            out.push_back(static_cast<PackEntry&&>(pe));
        }
        return true;
    }

    void fourcc_str(crd::u32 fourcc, char out[5])
    {
        out[0] = static_cast<char>(fourcc & 0xFFU);
        out[1] = static_cast<char>((fourcc >> 8U) & 0xFFU);
        out[2] = static_cast<char>((fourcc >> 16U) & 0xFFU);
        out[3] = static_cast<char>((fourcc >> 24U) & 0xFFU);
        out[4] = '\0';
    }

    // TIML enum bytes → the OTIO import model (flat, no nested ternaries under the tidy gate)
    [[nodiscard]] crd::assetio::OtioTrackKind track_kind_of(crd::u8 kind)
    {
        if (kind == static_cast<crd::u8>(crd::timeline::TrackKind::Video))
        {
            return crd::assetio::OtioTrackKind::Video;
        }
        if (kind == static_cast<crd::u8>(crd::timeline::TrackKind::Audio))
        {
            return crd::assetio::OtioTrackKind::Audio;
        }
        return crd::assetio::OtioTrackKind::Other;
    }

    [[nodiscard]] crd::assetio::OtioItemType item_type_of(crd::u8 type)
    {
        if (type == static_cast<crd::u8>(crd::timeline::ItemType::Clip))
        {
            return crd::assetio::OtioItemType::Clip;
        }
        if (type == static_cast<crd::u8>(crd::timeline::ItemType::Gap))
        {
            return crd::assetio::OtioItemType::Gap;
        }
        return crd::assetio::OtioItemType::Transition;
    }
} // namespace

crd::containers::String verb_import(const char* path, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> bytes(alloc);
    if (path == nullptr || !read_file(path, bytes)) { return fail(alloc, "import", "cannot read file"); }

    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "import");
    w.kv("path", path);

    if (ends_with(path, ".glb") || ends_with(path, ".gltf") || ends_with(path, ".stl") ||
        ends_with(path, ".obj") || ends_with(path, ".ply") || ends_with(path, ".3mf"))
    {
        crd::assetio::ImportedAsset asset(alloc);
        crd::assetio::ImportStatus  st   = crd::assetio::ImportStatus::Unsupported;
        const auto                  span = crd::containers::as_const_span(bytes);
        if (ends_with(path, ".glb")) { st = crd::assetio::parse_glb(span, alloc, asset); }
        else if (ends_with(path, ".gltf")) { st = crd::assetio::parse_gltf(span, {}, alloc, asset); }
        else if (ends_with(path, ".stl")) { st = crd::assetio::parse_stl(span, alloc, asset); }
        else if (ends_with(path, ".obj")) { st = crd::assetio::parse_obj(span, alloc, asset); }
        else if (ends_with(path, ".ply")) { st = crd::assetio::parse_ply(span, alloc, asset); }
        else // .3mf: the OPC archive → 3D/3dmodel.model → the XML model parse (the cook's own route)
        {
            crd::resources::ZipReader zip(alloc);
            if (zip.open(span) != crd::resources::ZipError::Ok)
            {
                return fail(alloc, "import", "not a readable 3MF archive");
            }
            const crd::i64 idx = zip.find("3D/3dmodel.model");
            crd::containers::Array<crd::u8> model(alloc);
            if (idx < 0 || zip.extract(static_cast<crd::usize>(idx), model) != crd::resources::ZipError::Ok)
            {
                return fail(alloc, "import", "3MF archive has no readable 3D/3dmodel.model");
            }
            st = crd::assetio::parse_3mf_model(crd::containers::as_const_span(model), alloc, asset);
        }
        if (st != crd::assetio::ImportStatus::Ok)
        {
            return fail(alloc, "import", crd::assetio::import_status_name(st));
        }
        w.kv("ok", true);
        w.kv("format", "mesh-asset");
        w.kv("meshes", static_cast<crd::u64>(asset.meshes.size()));
        w.kv("nodes", static_cast<crd::u64>(asset.nodes.size()));
        w.kv("materials", static_cast<crd::u64>(asset.materials.size()));
        w.kv("skins", static_cast<crd::u64>(asset.skins.size()));
        w.kv("animations", static_cast<crd::u64>(asset.animations.size()));
        w.key("mesh_names");
        w.begin_array();
        for (const auto& m : asset.meshes) { w.value_string(m.name.c_str()); }
        w.end_array();
    }
    else if (ends_with(path, ".otio"))
    {
        crd::assetio::ImportedTimeline tl(alloc);
        crd::assetio::OtioDiag         diag;
        if (crd::assetio::otio_parse(crd::containers::as_const_span(bytes), tl, &diag) !=
            crd::assetio::OtioResult::Ok)
        {
            return fail(alloc, "import", diag.detail);
        }
        w.kv("ok", true);
        w.kv("format", "timeline");
        w.kv("name", tl.name.c_str());
        w.kv("tracks", static_cast<crd::u64>(tl.tracks.size()));
        w.kv("items", static_cast<crd::u64>(tl.items.size()));
        w.kv("markers", static_cast<crd::u64>(tl.markers.size()));
    }
    else if (ends_with(path, ".wav") || ends_with(path, ".aiff") || ends_with(path, ".aif") ||
             ends_with(path, ".flac"))
    {
        crd::audio::AudioPcm pcm(alloc);
        bool                 ok = false;
        if (ends_with(path, ".wav"))
        {
            ok = crd::audio::wav_decode(crd::containers::as_const_span(bytes), pcm) == crd::audio::WavError::Ok;
        }
        else if (ends_with(path, ".flac"))
        {
            ok = crd::audio::flac_decode(crd::containers::as_const_span(bytes), pcm) ==
                 crd::audio::FlacError::Ok;
        }
        else
        {
            ok = crd::audio::aiff_decode(crd::containers::as_const_span(bytes), pcm) ==
                 crd::audio::AiffError::Ok;
        }
        if (!ok) { return fail(alloc, "import", "audio decode failed"); }
        w.kv("ok", true);
        w.kv("format", "audio");
        w.kv("sample_rate", pcm.sample_rate);
        w.kv("channels", static_cast<crd::u32>(pcm.channels));
        w.kv("frames", pcm.frame_count());
    }
    else if (ends_with(path, ".mid"))
    {
        crd::audio::MidiResource midi(alloc);
        if (crd::audio::midi_parse_smf(crd::containers::as_const_span(bytes), midi) != crd::audio::MidiError::Ok)
        {
            return fail(alloc, "import", "SMF parse failed");
        }
        w.kv("ok", true);
        w.kv("format", "midi");
        w.kv("notes", static_cast<crd::u64>(midi.notes.size()));
        w.kv("tempo_changes", static_cast<crd::u64>(midi.tempo.size()));
    }
    else
    {
        return fail(alloc, "import", "unsupported extension");
    }
    w.end_object();
    return crd::containers::String(w.str());
}

crd::containers::String verb_cook(const char* root, const char* out_pack, crd::memory::IAllocator* alloc)
{
    if (root == nullptr || out_pack == nullptr) { return fail(alloc, "cook", "root and out are required"); }
    if (!fs::is_directory(fs::Path(crd::containers::StringView(root))))
    {
        return fail(alloc, "cook", "root is not a directory");
    }
    const int rc = crd::cooker::cmd_cook(root, out_pack);
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "cook");
    w.kv("ok", rc == 0);
    w.kv("pack", out_pack);
    w.kv("exit_code", static_cast<crd::i64>(rc));
    w.end_object();
    return crd::containers::String(w.str());
}

crd::containers::String verb_query(const char* pack_path, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<PackEntry> entries(alloc);
    if (pack_path == nullptr || !read_pack_entries(pack_path, alloc, entries))
    {
        return fail(alloc, "query", "not a readable PACK");
    }
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "query");
    w.kv("ok", true);
    w.kv("pack", pack_path);
    w.kv("count", static_cast<crd::u64>(entries.size()));
    w.key("entries");
    w.begin_array();
    for (const PackEntry& e : entries)
    {
        w.begin_object();
        const crd::containers::String id = e.id.to_string(alloc);
        w.kv("uuid", id.c_str());
        char type[5];
        fourcc_str(e.type_fourcc, type);
        w.kv("type", type);
        w.kv("name", e.name.c_str());
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return crd::containers::String(w.str());
}

crd::containers::String verb_instantiate(const char* pack_path, const char* asset_name,
                                         const crd::f32 translate[3], bool dry_run, const char* out_scene,
                                         crd::memory::IAllocator* alloc)
{
    // ⛔ VALIDATE EVERYTHING before the first side effect (the transactional contract)
    if (pack_path == nullptr || asset_name == nullptr || out_scene == nullptr || translate == nullptr)
    {
        return fail(alloc, "instantiate", "missing arguments");
    }
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(translate[i])) { return fail(alloc, "instantiate", "non-finite transform"); }
    }
    crd::containers::Array<PackEntry> entries(alloc);
    if (!read_pack_entries(pack_path, alloc, entries)) { return fail(alloc, "instantiate", "not a readable PACK"); }
    const PackEntry* found = nullptr;
    for (const PackEntry& e : entries)
    {
        if (e.type_fourcc == crd::resources::kFourCC_MESH && std::strcmp(e.name.c_str(), asset_name) == 0)
        {
            found = &e;
            break;
        }
    }
    if (found == nullptr)
    {
        return fail(alloc, "instantiate", "no MESH with that name in the pack"); // atomic: nothing written
    }

    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "instantiate");
    w.kv("ok", true);
    const crd::containers::String id = found->id.to_string(alloc);
    w.kv("mesh_uuid", id.c_str());
    if (dry_run)
    {
        w.kv("applied", false);
        w.kv("would_apply", true);
        w.end_object();
        return crd::containers::String(w.str());
    }

    // apply: World → entity(Transform + MeshRenderer) → SCEN bytes → file
    crd::scene::World world(alloc);
    world.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    crd::scene::register_render_components(world);
    const crd::scene::EntityId entity = world.spawn();
    crd::scene::Transform      t;
    t.translation = crd::math::from_raw_vec<crd::units::dim::Length>(
        crd::math::Vec3f{translate[0], translate[1], translate[2]});
    t.world = crd::math::from_trs(crd::math::Vec3f{translate[0], translate[1], translate[2]},
                                  crd::math::Quatf::identity(), crd::math::Vec3f{1.0F, 1.0F, 1.0F});
    world.add_component(entity, t);
    crd::scene::MeshRenderer mr;
    mr.mesh = found->id;
    world.add_component(entity, mr);

    crd::scene::SceneArtifactBuilder      builder(alloc, crd::resources::ResourceId::mint_random());
    const crd::containers::Array<crd::u8> scen = builder.build(world);
    if (scen.size() == 0 ||
        !fs::write_file_binary(fs::Path(crd::containers::StringView(out_scene)),
                               crd::containers::as_const_span(scen)))
    {
        return fail(alloc, "instantiate", "SCEN build/write failed");
    }
    w.kv("applied", true);
    w.kv("scene", out_scene);
    w.kv("entities", 1U);
    w.end_object();
    return crd::containers::String(w.str());
}

crd::containers::String verb_sequence(const char* name, const char* clip_a, crd::i64 frames_a,
                                      const char* clip_b, crd::i64 frames_b, crd::i64 transition_frames,
                                      const char* out_timl, const char* out_otio,
                                      crd::memory::IAllocator* alloc)
{
    if (name == nullptr || clip_a == nullptr || clip_b == nullptr || out_timl == nullptr ||
        out_otio == nullptr)
    {
        return fail(alloc, "sequence", "missing arguments");
    }
    if (frames_a <= 0 || frames_b <= 0 || transition_frames < 0 ||
        transition_frames / 2 >= frames_a || transition_frames / 2 >= frames_b)
    {
        return fail(alloc, "sequence", "durations must be positive and cover the transition");
    }

    const crd::time::RationalRate rate = crd::time::kRate24;
    crd::timeline::TimelineResource tl(alloc);
    tl.name_off = tl.intern(name);

    crd::timeline::MediaRec media_a;
    media_a.kind     = static_cast<crd::u8>(crd::timeline::MediaKind::Missing);
    media_a.name_off = tl.intern(clip_a);
    tl.media.push_back(media_a);
    crd::timeline::MediaRec media_b;
    media_b.kind     = static_cast<crd::u8>(crd::timeline::MediaKind::Missing);
    media_b.name_off = tl.intern(clip_b);
    tl.media.push_back(media_b);

    crd::timeline::ItemRec item_a;
    item_a.type             = static_cast<crd::u8>(crd::timeline::ItemType::Clip);
    item_a.name_off         = tl.intern(clip_a);
    item_a.has_source_range = 1;
    item_a.source_range     = {{0, rate}, {frames_a, rate}};
    item_a.media_ref        = 0;
    tl.items.push_back(item_a);
    if (transition_frames > 0)
    {
        crd::timeline::ItemRec tr;
        tr.type                = static_cast<crd::u8>(crd::timeline::ItemType::Transition);
        tr.name_off            = tl.intern("dissolve");
        tr.transition_type_off = tl.intern("SMPTE_Dissolve");
        tr.in_offset           = {transition_frames / 2, rate};
        tr.out_offset          = {transition_frames - transition_frames / 2, rate};
        tl.items.push_back(tr);
    }
    crd::timeline::ItemRec item_b;
    item_b.type             = static_cast<crd::u8>(crd::timeline::ItemType::Clip);
    item_b.name_off         = tl.intern(clip_b);
    item_b.has_source_range = 1;
    item_b.source_range     = {{0, rate}, {frames_b, rate}};
    item_b.media_ref        = 1;
    tl.items.push_back(item_b);

    crd::timeline::TrackRec track;
    track.kind          = static_cast<crd::u8>(crd::timeline::TrackKind::Video);
    track.kind_name_off = tl.intern("Video");
    track.name_off      = tl.intern("V1");
    track.first_item    = 0;
    track.item_count    = static_cast<crd::u32>(tl.items.size());
    tl.tracks.push_back(track);

    const crd::containers::Array<crd::u8> timl =
        crd::timeline::timeline_build(tl, crd::resources::ResourceId::mint_random(), alloc);
    if (timl.size() == 0) { return fail(alloc, "sequence", "timeline failed validation"); }

    // the `.otio` twin through the SAME translation the cook uses (resource → imported → export)
    crd::assetio::ImportedTimeline imported(alloc);
    imported.name = name;
    for (crd::usize m = 0; m < tl.media.size(); ++m)
    {
        crd::assetio::ImportedMediaRef ref(alloc);
        ref.kind = crd::assetio::OtioMediaKind::Missing;
        ref.name = tl.str(tl.media[m].name_off);
        imported.media.push_back(static_cast<crd::assetio::ImportedMediaRef&&>(ref));
    }
    for (crd::usize i = 0; i < tl.items.size(); ++i)
    {
        const crd::timeline::ItemRec&      rec = tl.items[i];
        crd::assetio::ImportedTimelineItem item(alloc);
        item.type = rec.type == static_cast<crd::u8>(crd::timeline::ItemType::Clip)
                        ? crd::assetio::OtioItemType::Clip
                        : crd::assetio::OtioItemType::Transition;
        item.name             = tl.str(rec.name_off);
        item.has_source_range = rec.has_source_range != 0;
        item.source_range     = rec.source_range;
        item.media_ref        = rec.media_ref;
        item.in_offset        = rec.in_offset;
        item.out_offset       = rec.out_offset;
        item.transition_type  = tl.str(rec.transition_type_off);
        imported.items.push_back(static_cast<crd::assetio::ImportedTimelineItem&&>(item));
    }
    crd::assetio::ImportedTrack video(alloc);
    video.kind       = crd::assetio::OtioTrackKind::Video;
    video.kind_name  = "Video";
    video.name       = "V1";
    video.first_item = 0;
    video.item_count = static_cast<crd::u32>(imported.items.size());
    imported.tracks.push_back(static_cast<crd::assetio::ImportedTrack&&>(video));

    const crd::containers::String otio = crd::assetio::otio_export(imported, alloc);
    if (!fs::write_file_binary(fs::Path(crd::containers::StringView(out_timl)),
                               crd::containers::as_const_span(timl)) ||
        !fs::write_file_text(fs::Path(crd::containers::StringView(out_otio)),
                             crd::containers::StringView(otio.c_str(), otio.size())))
    {
        return fail(alloc, "sequence", "write failed");
    }

    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "sequence");
    w.kv("ok", true);
    w.kv("timl", out_timl);
    w.kv("otio", out_otio);
    w.kv("duration_frames", static_cast<crd::i64>(frames_a + frames_b));
    w.end_object();
    return crd::containers::String(w.str());
}

namespace
{
    // deterministic solid take per media index — the renderer band binds real renders through this same seam
    bool solid_resolve(void* /*user*/, const crd::timeline::TimelineResource& tl,
                       const crd::timeline::ActiveClip& clip, crd::resources::HdrImage& out)
    {
        const crd::u32 m = clip.media_ref == crd::timeline::kInvalidIndex ? 0 : clip.media_ref;
        (void)tl;
        const crd::f32 palette[4][3] = {
            {0.9F, 0.3F, 0.1F}, {0.1F, 0.4F, 0.9F}, {0.2F, 0.8F, 0.3F}, {0.8F, 0.8F, 0.2F}};
        const crd::f32* c = palette[m % 4U];
        for (crd::u32 y = 0; y < out.height; ++y)
        {
            for (crd::u32 x = 0; x < out.width; ++x)
            {
                out.at(x, y, 0) = c[0];
                out.at(x, y, 1) = c[1];
                out.at(x, y, 2) = c[2];
            }
        }
        return true;
    }

    struct RenderSink
    {
        const char*              dir;
        crd::memory::IAllocator* alloc;
        crd::i64                 written = 0;
        bool                     failed  = false;
    };

    bool sink_frame(void* user, crd::i64 frame, crd::containers::ConstSpan<crd::u8> exr)
    {
        auto* s = static_cast<RenderSink*>(user);
        char  path[512];
        std::snprintf(path, sizeof(path), "%s/f%04lld.exr", s->dir, static_cast<long long>(frame));
        if (!fs::write_file_binary(fs::Path(crd::containers::StringView(path)), exr))
        {
            s->failed = true;
            return false;
        }
        ++s->written;
        return true;
    }
} // namespace

crd::containers::String verb_render(const char* otio_path, const char* out_dir, crd::i64 max_frames,
                                    crd::memory::IAllocator* alloc)
{
    if (otio_path == nullptr || out_dir == nullptr) { return fail(alloc, "render", "missing arguments"); }
    crd::containers::Array<crd::u8> bytes(alloc);
    if (!read_file(otio_path, bytes)) { return fail(alloc, "render", "cannot read timeline"); }
    crd::assetio::ImportedTimeline imported(alloc);
    crd::assetio::OtioDiag         diag;
    if (crd::assetio::otio_parse(crd::containers::as_const_span(bytes), imported, &diag) !=
        crd::assetio::OtioResult::Ok)
    {
        return fail(alloc, "render", diag.detail);
    }
    // translate (the cook handler's own path shape, minimal here: resolved clips only)
    crd::timeline::TimelineResource tl(alloc);
    tl.name_off = tl.intern(imported.name.c_str());
    for (const crd::assetio::ImportedMediaRef& m : imported.media)
    {
        crd::timeline::MediaRec rec;
        rec.kind     = static_cast<crd::u8>(crd::timeline::MediaKind::Missing);
        rec.name_off = tl.intern(m.name.c_str());
        tl.media.push_back(rec);
    }
    for (const crd::assetio::ImportedTrack& track : imported.tracks)
    {
        crd::timeline::TrackRec tr;
        tr.kind = track.kind == crd::assetio::OtioTrackKind::Video
                      ? static_cast<crd::u8>(crd::timeline::TrackKind::Video)
                      : static_cast<crd::u8>(crd::timeline::TrackKind::Audio);
        tr.name_off   = tl.intern(track.name.c_str());
        tr.first_item = static_cast<crd::u32>(tl.items.size());
        for (crd::u32 i = 0; i < track.item_count; ++i)
        {
            const crd::assetio::ImportedTimelineItem& src = imported.items[track.first_item + i];
            crd::timeline::ItemRec                    rec;
            crd::timeline::ItemType                   it = crd::timeline::ItemType::Transition;
            if (src.type == crd::assetio::OtioItemType::Clip) { it = crd::timeline::ItemType::Clip; }
            else if (src.type == crd::assetio::OtioItemType::Gap) { it = crd::timeline::ItemType::Gap; }
            rec.type                = static_cast<crd::u8>(it);
            rec.name_off            = tl.intern(src.name.c_str());
            rec.has_source_range    = src.has_source_range ? 1 : 0;
            rec.source_range        = src.source_range;
            rec.media_ref           = src.media_ref;
            rec.in_offset           = src.in_offset;
            rec.out_offset          = src.out_offset;
            rec.transition_type_off = tl.intern(src.transition_type.c_str());
            tl.items.push_back(rec);
        }
        tr.item_count = static_cast<crd::u32>(tl.items.size()) - tr.first_item;
        tl.tracks.push_back(tr);
    }
    const crd::time::RationalTime dur = crd::timeline::timeline_duration(tl);
    const crd::time::RationalTime len24 =
        crd::time::rescaled_to(dur, crd::time::kRate24, crd::time::RescaleRounding::Round);
    crd::i64 frames = len24.value;
    if (max_frames > 0 && frames > max_frames) { frames = max_frames; }
    if (frames <= 0) { return fail(alloc, "render", "the timeline is empty"); }

    if (!fs::is_directory(fs::Path(crd::containers::StringView(out_dir))) &&
        !fs::create_directories(fs::Path(crd::containers::StringView(out_dir))))
    {
        return fail(alloc, "render", "cannot create output directory");
    }

    crd::timeline::RenderConfig cfg;
    cfg.frame_rate  = crd::time::kRate24;
    cfg.start       = {0, crd::time::kRate24};
    cfg.frame_count = frames;
    cfg.width       = 64;
    cfg.height      = 36;
    cfg.pixel_type  = crd::resources::ExrPixelType::Half;
    cfg.compression = crd::resources::ExrCompression::Zip;

    RenderSink sink{out_dir, alloc, 0, false};
    const crd::i64 done =
        crd::timeline::render_exr_sequence(tl, cfg, &solid_resolve, nullptr, &sink_frame, &sink, alloc);

    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "render");
    w.kv("ok", done == frames && !sink.failed);
    w.kv("frames", static_cast<crd::i64>(done));
    w.kv("dir", out_dir);
    w.end_object();
    return crd::containers::String(w.str());
}

crd::containers::String verb_export_timeline(const char* timl_path, const char* out_otio,
                                             crd::memory::IAllocator* alloc)
{
    if (timl_path == nullptr || out_otio == nullptr)
    {
        return fail(alloc, "export", "missing arguments");
    }
    crd::containers::Array<crd::u8> bytes(alloc);
    if (!read_file(timl_path, bytes)) { return fail(alloc, "export", "cannot read TIML"); }
    crd::timeline::TimelineLoader loader(alloc);
    crd::resources::LoadContext   ctx;
    ctx.bytes     = crd::containers::as_const_span(bytes);
    ctx.allocator = alloc;
    auto* tl      = static_cast<crd::timeline::TimelineResource*>(loader.load(ctx));
    if (tl == nullptr) { return fail(alloc, "export", "not a valid TIML artifact"); }

    crd::assetio::ImportedTimeline imported(alloc);
    imported.name = tl->name();
    for (const crd::timeline::MediaRec& m : tl->media)
    {
        crd::assetio::ImportedMediaRef ref(alloc);
        switch (static_cast<crd::timeline::MediaKind>(m.kind))
        {
        case crd::timeline::MediaKind::External: ref.kind = crd::assetio::OtioMediaKind::External; break;
        case crd::timeline::MediaKind::ImageSequence:
            ref.kind = crd::assetio::OtioMediaKind::ImageSequence;
            break;
        case crd::timeline::MediaKind::Missing:
        case crd::timeline::MediaKind::Resource:
        default: ref.kind = crd::assetio::OtioMediaKind::Missing; break;
        }
        ref.name                = tl->str(m.name_off);
        ref.url                 = tl->str(m.url_off);
        ref.name_prefix         = tl->str(m.prefix_off);
        ref.name_suffix         = tl->str(m.suffix_off);
        ref.has_available_range = m.has_available_range != 0;
        ref.available_range     = m.available_range;
        ref.start_frame         = m.start_frame;
        ref.frame_step          = m.frame_step;
        ref.zero_padding        = m.zero_padding;
        ref.seq_rate            = m.seq_rate;
        imported.media.push_back(static_cast<crd::assetio::ImportedMediaRef&&>(ref));
    }
    for (const crd::timeline::TrackRec& track : tl->tracks)
    {
        crd::assetio::ImportedTrack out_track(alloc);
        out_track.kind       = track_kind_of(track.kind);
        out_track.kind_name  = tl->str(track.kind_name_off);
        out_track.name       = tl->str(track.name_off);
        out_track.first_item = static_cast<crd::u32>(imported.items.size());
        for (crd::u32 i = 0; i < track.item_count; ++i)
        {
            const crd::timeline::ItemRec&      rec = tl->items[track.first_item + i];
            crd::assetio::ImportedTimelineItem item(alloc);
            item.type             = item_type_of(rec.type);
            item.name             = tl->str(rec.name_off);
            item.has_source_range = rec.has_source_range != 0;
            item.source_range     = rec.source_range;
            item.media_ref        = rec.media_ref;
            item.in_offset        = rec.in_offset;
            item.out_offset       = rec.out_offset;
            item.transition_type  = tl->str(rec.transition_type_off);
            imported.items.push_back(static_cast<crd::assetio::ImportedTimelineItem&&>(item));
        }
        out_track.item_count = static_cast<crd::u32>(imported.items.size()) - out_track.first_item;
        imported.tracks.push_back(static_cast<crd::assetio::ImportedTrack&&>(out_track));
    }
    const crd::containers::String otio = crd::assetio::otio_export(imported, alloc);
    loader.unload(tl);
    if (!fs::write_file_text(fs::Path(crd::containers::StringView(out_otio)),
                             crd::containers::StringView(otio.c_str(), otio.size())))
    {
        return fail(alloc, "export", "write failed");
    }
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", "export");
    w.kv("ok", true);
    w.kv("otio", out_otio);
    w.end_object();
    return crd::containers::String(w.str());
}

} // namespace crd::ceridc
