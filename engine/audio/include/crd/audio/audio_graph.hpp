#pragma once

// audio_graph.hpp — GEO-10: the OFFLINE graph renderer — deterministic block processing of an
// AudioGraphResource: Sources pull bound buffers, Gains/Sends apply (automated) dB, Biquads run RBJ filters
// (hesap-dsp closed forms, f64 state), Mixes sum — all on a STEREO f32 bus (mono sources center). Identical
// inputs render identical bytes (the gate), which is also why automation evaluates PER SAMPLE in exact
// rational time (the GEO-9 discipline) — a block-boundary ramp would make the result depend on block size.
//
// The realtime layer (audio_device.hpp) runs voices + commands, not this renderer — offline render is the
// mastering path (GEO-9 timeline audio masters land here too).

#include <crd/audio/audio_resources.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::audio
{

// per-NODE-index buffer binding (only Source nodes read theirs; others may be empty)
struct GraphSourceBinding
{
    crd::containers::ConstSpan<crd::f32> samples; // interleaved at `channels`
    crd::u32                             channels = 0;
};

// Render `frames` frames of the graph's output node into `out` (resized: frames × 2, interleaved stereo).
// Returns frames rendered (0 on an invalid graph / missing source binding — never a partial buffer).
[[nodiscard]] crd::i64 render_graph(const AudioGraphResource& graph,
                                    crd::containers::ConstSpan<GraphSourceBinding> bindings, crd::i64 frames,
                                    crd::containers::Array<crd::f32>& out);

} // namespace crd::audio
