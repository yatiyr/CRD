#pragma once

// crd-ceir — the audio dialect's find_audio_misuse verifier (CEIR-31a-1a, sec-142; route the GEO-10 offline audio graph
// through CEIR, proof = BIT-EXACT vs render_graph). The GENERATED verify_source/gain/biquad/mix/send (gen/audio_ops.hpp)
// own each op's STRUCTURAL contract (operand/result counts + required-attr PRESENCE + KIND — `name` Symbol, `filter`/
// `start_frame`/`loop` Int, `cutoff`/`q`/`gain_db` Float); THIS owns the SEMANTIC + shape rules, mirroring the shared
// AGRF graph_validate (audio_resources.cpp): an audio op's operand(s)+result must be Tensor-kinded (OperandNotTensor);
// gain/biquad/send/mix PRESERVE the buffer shape so result type == input type (ResultTypeMismatch); a biquad `filter` is
// in [0,3]=Lowpass/Highpass/Bandpass/Notch (BiquadFilterInvalid), `cutoff` in (0,1) exclusive (BiquadCutoffInvalid),
// `q` > 0 (BiquadQInvalid); a source `start_frame` >= 0 (SourceStartFrameInvalid). The find_dist/tensor_misuse house
// pattern. ⛔ I6 — matches op NAME (ctx.op_name, const), never op.kind. ⛔ const Context& — reads names/attrs/types and
// COMPARES TypeIds, interns NOTHING. ⛔ the DAG/cycle + one-output rules are the CEIR STRUCTURE verifier's (a combinational
// feedback loop is FeedbackWithoutState via the biquad's OpTrait::StateEdge — the 9b DAW form), NOT find_audio_misuse's.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/audio_ops.hpp> // register_audio_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::audio
{
enum class AudioMisuseKind : u8
{
    None = 0,
    OperandNotTensor,      // an audio op's operand OR result is not Tensor-kinded
    ResultTypeMismatch,    // a gain/biquad/send/mix result type != input type (an audio op preserves the buffer shape)
    BiquadFilterInvalid,   // audio.biquad `filter` < 0 or > 3 (not Lowpass/Highpass/Bandpass/Notch) (or non-Int)
    BiquadCutoffInvalid,   // audio.biquad `cutoff` not in (0,1) exclusive (or non-Float)
    BiquadQInvalid,        // audio.biquad `q` <= 0 (or non-Float)
    SourceStartFrameInvalid, // audio.source `start_frame` < 0 (or non-Int)
    DelayFramesInvalid,      // audio.delay `delay_frames` < 0 (or non-Int)
    CompressorRatioInvalid,   // audio.compressor `ratio` < 1 (or non-Float)
    CompressorAttackInvalid,  // audio.compressor `attack_ms` <= 0 (or non-Float)
    CompressorReleaseInvalid, // audio.compressor `release_ms` <= 0 (or non-Float)
    SampleRateInvalid,        // func.func `sample_rate` <= 0 (or non-Int) -- the graph-global rate audio.compressor needs
    // ⛔ CEIR-31z ruling (5): the audio buffer's SHAPE is a declared header word -- validate it at cook time (the
    // declared-words-must-be-validated family). A stereo audio value is a rank-2 Tensor [frames, 2]: dim1 (channels) MUST
    // be Static == 2; dim0 (frames) is the runtime block size -- Static >= 1 (a pinned block) OR Dynamic (any block), NEVER
    // Symbolic/0. (The executor's frames-vs-pinned-dim0 EQUALITY is an execute-time check, execute_audio_graph_ceir.)
    TensorRankInvalid,   // an audio value's Tensor shape is not rank-2 ([frames, channels])
    ChannelCountInvalid, // an audio value's channel dim (dim1) is not Static == 2 (stereo)
    FrameDimInvalid,     // an audio value's frame dim (dim0) is Static 0 or Symbolic (not a Static>=1 or Dynamic block size)
};
[[nodiscard]] containers::StringView audio_misuse_kind_name(AudioMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op`, and the `value` it points at (the operand/result
// for a tensor/type misuse; null for an attr misuse — the dist/tensor misuse pointing convention).
struct AudioMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    AudioMisuseKind  kind  = AudioMisuseKind::None;
};
// The FIRST audio misuse in module `m` (pre-order), or {None}. ⛔ const Context& — reads op names/attrs/types, interns nothing.
[[nodiscard]] AudioMisuse find_audio_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::audio
