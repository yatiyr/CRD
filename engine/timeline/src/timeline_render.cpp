// timeline_render.cpp — GEO-9: the EXR-sequence render driver (see timeline_render.hpp).

#include <crd/timeline/timeline_render.hpp>

namespace crd::timeline
{

crd::i64 render_exr_sequence(const TimelineResource& tl, const RenderConfig& cfg, ResolveMediaFn resolve,
                             void* resolve_user, FrameSinkFn sink, void* sink_user,
                             crd::memory::IAllocator* alloc)
{
    if (!cfg.frame_rate.valid() || !cfg.start.valid() || cfg.frame_count <= 0 || cfg.width == 0 ||
        cfg.height == 0 || resolve == nullptr || sink == nullptr || alloc == nullptr)
    {
        return 0;
    }

    const crd::usize            texels = static_cast<crd::usize>(cfg.width) * cfg.height * 3U;
    crd::resources::HdrImage    canvas(alloc);
    crd::resources::HdrImage    layer(alloc);
    crd::resources::HdrImage    mixed(alloc);
    canvas.width  = layer.width = mixed.width = cfg.width;
    canvas.height = layer.height = mixed.height = cfg.height;
    canvas.channels = layer.channels = mixed.channels = 3;
    canvas.pixels.resize(texels);
    layer.pixels.resize(texels);
    mixed.pixels.resize(texels);

    crd::containers::Array<ActiveClip> active(alloc);

    for (crd::i64 f = 0; f < cfg.frame_count; ++f)
    {
        const crd::time::RationalTime t =
            crd::time::add(cfg.start, crd::time::RationalTime{f, cfg.frame_rate});

        for (crd::usize i = 0; i < texels; ++i) { canvas.pixels[i] = 0.0F; } // deterministic black
        active.clear();
        evaluate_tracks(tl, t, active);

        // OTIO stack order: track 0 is the BOTTOM — later video tracks overwrite where they have content
        for (crd::usize ti = 0; ti < tl.tracks.size(); ++ti)
        {
            if (tl.tracks[ti].kind != static_cast<crd::u8>(TrackKind::Video)) { continue; }
            bool track_has_content = false;
            for (crd::usize i = 0; i < texels; ++i) { mixed.pixels[i] = 0.0F; }
            for (const ActiveClip& clip : active)
            {
                if (clip.track_index != ti) { continue; }
                bool resolved = resolve(resolve_user, tl, clip, layer);
                if (!resolved)
                {
                    for (crd::usize i = 0; i < texels; ++i) { layer.pixels[i] = 0.0F; } // black, never a shift
                }
                for (crd::usize i = 0; i < texels; ++i) { mixed.pixels[i] += clip.weight * layer.pixels[i]; }
                track_has_content = true;
            }
            if (track_has_content)
            {
                for (crd::usize i = 0; i < texels; ++i) { canvas.pixels[i] = mixed.pixels[i]; }
            }
        }

        const crd::containers::Array<crd::u8> exr =
            crd::resources::hdr_encode_exr(canvas, cfg.pixel_type, cfg.compression, alloc);
        if (exr.size() == 0) { return f; }
        if (!sink(sink_user, f, crd::containers::as_const_span(exr))) { return f; }
    }
    return cfg.frame_count;
}

} // namespace crd::timeline
