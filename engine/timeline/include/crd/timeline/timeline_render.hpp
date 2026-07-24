#pragma once

// timeline_render.hpp — GEO-9: the EXR-SEQUENCE render driver — evaluate the timeline frame by frame at a
// rational frame rate and encode each composed frame with OUR EXR codec. Deterministic end to end: identical
// timeline + identical media bytes → identical EXR bytes (gated).
//
// The driver owns TIME and COMPOSITION; MEDIA comes through an injected resolver (the seam the renderer band
// plugs real scene renders into; tests inject synthetic takes; the cook/CLI plugs file readers). Composition
// v1: within a track, active clips mix by their dissolve weights (linear light — HdrImage IS linear float);
// across tracks, later VIDEO tracks overwrite earlier ones where they have content (OTIO's bottom-up stack;
// alpha-aware compositing is the compositor band's). Audio tracks are structure here — their master render is
// GEO-10's (crd-audio), recorded, not forgotten.
//
// File I/O stays with the caller (the codec doctrine): frames arrive as encoded byte spans through the sink.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/hdr_image.hpp>
#include <crd/time/rational_time.hpp>
#include <crd/timeline/timeline_eval.hpp>
#include <crd/timeline/timeline_resource.hpp>

namespace crd::timeline
{

// Fill `out` (already sized width×height×3 by the driver) with the media frame for `clip` — sampling the
// referenced media at `clip.media_time`. Return false when unresolvable (the driver then treats the clip as
// black, deterministically — a missing frame must not shift the cut).
using ResolveMediaFn = bool (*)(void* user, const TimelineResource& tl, const ActiveClip& clip,
                                crd::resources::HdrImage& out);

// Receive frame `frame_index`'s encoded EXR bytes. Return false to abort the render (reported in the count).
using FrameSinkFn = bool (*)(void* user, crd::i64 frame_index, crd::containers::ConstSpan<crd::u8> exr_bytes);

struct RenderConfig
{
    crd::time::RationalRate      frame_rate;  // the master's rate (frames step {1, frame_rate} — exact)
    crd::time::RationalTime      start;       // timeline-local start (usually 0)
    crd::i64                     frame_count = 0;
    crd::u32                     width       = 0;
    crd::u32                     height      = 0;
    crd::resources::ExrPixelType pixel_type  = crd::resources::ExrPixelType::Half;
    crd::resources::ExrCompression compression = crd::resources::ExrCompression::Zip;
};

// Render `cfg.frame_count` frames. Returns the number of frames delivered to the sink (== frame_count on
// success; fewer only on sink abort / encode failure / invalid config).
[[nodiscard]] crd::i64 render_exr_sequence(const TimelineResource& tl, const RenderConfig& cfg,
                                           ResolveMediaFn resolve, void* resolve_user, FrameSinkFn sink,
                                           void* sink_user, crd::memory::IAllocator* alloc);

} // namespace crd::timeline
