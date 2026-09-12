#pragma once

// ceir_audio_render.hpp — CEIR-31a-1b-ii (sec-142): the CEIR AUDIO EXECUTOR. Runs a `ceir.audio` graph (a CEIR Module of
// source/gain/biquad/mix over the 3d Tensor value) on the HOST by walking the func body in def-use/authoring order (a
// valid topological order for the DAG) and dispatching each op BY NAME (I6) to the SHARED per-node kernels
// (audio_kernels.hpp) render_graph uses -- so the output is BIT-EXACT vs render_graph BY CONSTRUCTION (the 30b-2b-1
// identical-kernel precedent). This is the "route audio through CEIR, NO new scheduler" proof: CEIR's order replaces
// render_graph's bespoke Kahn walk; the shared kernels prove the deletion changes nothing. ⛔ SINGLE-PASS over `frames`
// (the ceir.audio dialect has no automation node -> constant coeffs -> a single-pass biquad is bit-exact vs render_graph's
// 256-block chunking). Sources bind by `name` attr (CEIR has names; render_graph's AGRF binds positionally). The output
// sink = the unique audio op whose result is unconsumed (a CONVENTION; `audio.output` is the eventual contract).
// crd-audio depends on crd-ceir (a CONSUMER dep -- ADR-0109 constrains crd-ceir's OWN deps, not who consumes it).
// ⛔ CONTRACT: `module` is expected to have passed `find_audio_misuse == None` + the generated `verify_*` (well-formed);
// the executor STILL guards every attr read (`.valid()` + exact kind, the attr-reader scar) -- a present-but-wrong-kind
// or absent attr returns 0, never a silent degenerate coefficient.

#include <crd/audio/audio_graph.hpp> // GraphSourceBinding
#include <crd/ceir/context.hpp>      // Context, Module
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::audio
{

// a source binding keyed by the ceir.audio source's `name` attr (@a, @b, ...) -- the CEIR name model, not an AGRF index.
struct CeirSourceBinding
{
    crd::containers::StringView name;
    GraphSourceBinding          binding;
};

// Render `frames` frames of the CEIR audio graph in `module` into `out` (frames x 2 interleaved stereo). Returns frames
// rendered, or 0 on a malformed graph (no func / a non-audio op in the body / an operand that is not an earlier audio op
// / an unbound source / no unique sink). BIT-EXACT vs render_graph on the equivalent AGRF + bindings.
[[nodiscard]] crd::i64 execute_audio_graph_ceir(const crd::ceir::Context& ctx, const crd::ceir::Module& module,
                                                crd::containers::ConstSpan<CeirSourceBinding> bindings,
                                                crd::i64 frames, crd::containers::Array<crd::f32>& out);

} // namespace crd::audio
