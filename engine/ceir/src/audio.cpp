#include <crd/ceir/audio.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::audio
{
namespace
{
using containers::StringView;

// A type is Tensor-kinded (the 3d Tensor TypeKind — the audio buffer value).
[[nodiscard]] bool is_tensor_type(const Context& ctx, TypeId t) noexcept { return ctx.type_of(t).kind == TypeKind::Tensor; }

// ⛔ CEIR-31z ruling (5): validate an audio buffer's SHAPE (a declared header word — the declared-words-must-be-validated
// family). `t` is already known Tensor-kinded. A stereo audio value is a rank-2 Tensor [frames, 2]: dim1 (channels) MUST be
// Static == 2; dim0 (frames) is the runtime block size — Static >= 1 (a pinned block) OR Dynamic (any block), NEVER Symbolic
// or Static 0. (The executor enforces frames == a PINNED dim0 at execute time; here we own the cook-time structural shape.)
[[nodiscard]] AudioMisuseKind audio_shape_kind(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    if (tt.members.size() < 2U) { return AudioMisuseKind::TensorRankInvalid; } // a Tensor missing its element+shape pair
    const Type sh = ctx.type_of(tt.members[1U]);                               // members[0] = element, members[1] = shape
    if (sh.members.size() != 2U) { return AudioMisuseKind::TensorRankInvalid; }
    const Type d1 = ctx.type_of(sh.members[1U]); // dim1 = channels
    if (static_cast<DimKind>(d1.cols) != DimKind::Static || d1.count != 2U) { return AudioMisuseKind::ChannelCountInvalid; }
    const Type    d0 = ctx.type_of(sh.members[0U]); // dim0 = frames (the runtime block size)
    const DimKind k0 = static_cast<DimKind>(d0.cols);
    if (k0 == DimKind::Dynamic) { return AudioMisuseKind::None; }
    if (k0 == DimKind::Static && d0.count >= 1U) { return AudioMisuseKind::None; }
    return AudioMisuseKind::FrameDimInvalid; // Symbolic, or Static 0
}

// Per-op semantic checks (NOT recursive). No module-wide resolution — a source `name` is a binding key, not a cross-op ref.
AudioMisuse check_op(const Context& ctx, const Operation* op)
{
    const StringView nm = ctx.op_name(op->kind());

    if (nm == StringView("func.func"))
    {
        // the graph-global `sample_rate` (an OPTIONAL func attr the executor reads for audio.compressor's ms->coeff, rides
        // the GRAPH not the exec call -- the AudioGraphResource.sample_rate precedent, the recursion-policy func-attr idiom).
        // ABSENT -> defaults to 48000 (NOT a misuse); present-but-non-Int-or-<=0 -> SampleRateInvalid (check .valid() first).
        const AttrId sra = op->attr("sample_rate");
        if (!sra.valid()) { return {}; }
        const AttrValue sr = ctx.attr_value(sra);
        if (sr.kind != AttrKind::Int || sr.i <= 0) { return {nullptr, op, AudioMisuseKind::SampleRateInvalid}; }
        return {};
    }

    if (nm == StringView("audio.source"))
    {
        if (op->num_results() < 1U) { return {}; } // structural — the generated verify_source owns it
        const Value* const res = op->result(0U);
        if (!is_tensor_type(ctx, res->type())) { return {res, op, AudioMisuseKind::OperandNotTensor}; }
        const AudioMisuseKind ssk = audio_shape_kind(ctx, res->type()); // ⛔ 31z (5): [frames, 2] shape
        if (ssk != AudioMisuseKind::None) { return {res, op, ssk}; }
        const AttrId sfa = op->attr("start_frame");
        if (!sfa.valid()) { return {}; } // ABSENT -> generated verify_source owns PRESENCE (absent reads as Int 0)
        const AttrValue sf = ctx.attr_value(sfa);
        if (sf.kind != AttrKind::Int || sf.i < 0) { return {nullptr, op, AudioMisuseKind::SourceStartFrameInvalid}; }
        return {};
    }

    const bool is_gain       = nm == StringView("audio.gain");
    const bool is_send       = nm == StringView("audio.send");
    const bool is_biquad     = nm == StringView("audio.biquad");
    const bool is_mix        = nm == StringView("audio.mix");
    const bool is_delay      = nm == StringView("audio.delay");
    const bool is_compressor = nm == StringView("audio.compressor");
    if (!is_gain && !is_send && !is_biquad && !is_mix && !is_delay && !is_compressor) { return {}; }
    if (op->num_operands() < 1U || op->num_results() < 1U) { return {}; } // structural — the generated verifier owns it

    // operand(s) Tensor-kinded, then result Tensor-kinded, then result type == each operand type (an audio op PRESERVES the
    // buffer shape; for mix the variadic tail must all match — the same-shape sum contract). The find_dist_misuse order.
    const Value* const res = op->result(0U);
    for (u32 i = 0; i < op->num_operands(); ++i)
    {
        if (!is_tensor_type(ctx, op->operand(i)->type())) { return {op->operand(i), op, AudioMisuseKind::OperandNotTensor}; }
    }
    if (!is_tensor_type(ctx, res->type())) { return {res, op, AudioMisuseKind::OperandNotTensor}; }
    for (u32 i = 0; i < op->num_operands(); ++i)
    {
        if (res->type() != op->operand(i)->type()) { return {res, op, AudioMisuseKind::ResultTypeMismatch}; }
    }
    // ⛔ 31z (5): the buffer SHAPE ([frames, 2]). res == every operand (enforced just above), so res covers the operands.
    const AudioMisuseKind shk = audio_shape_kind(ctx, res->type());
    if (shk != AudioMisuseKind::None) { return {res, op, shk}; }

    if (is_biquad)
    {
        // ABSENT attr -> generated verify_biquad owns PRESENCE; check .valid() first so the KIND branch fires only on a
        // present-but-wrong-kind attr (absent reads as Int 0 -- the attr-reader-check-valid scar).
        const AttrId fla = op->attr("filter");
        if (!fla.valid()) { return {}; }
        const AttrValue fl = ctx.attr_value(fla);
        if (fl.kind != AttrKind::Int || fl.i < 0 || fl.i > 3) { return {nullptr, op, AudioMisuseKind::BiquadFilterInvalid}; }
        const AttrId cuta = op->attr("cutoff");
        if (!cuta.valid()) { return {}; }
        const AttrValue cut = ctx.attr_value(cuta);
        if (cut.kind != AttrKind::Float || !(cut.as_float() > 0.0) || !(cut.as_float() < 1.0))
        {
            return {nullptr, op, AudioMisuseKind::BiquadCutoffInvalid};
        }
        const AttrId qa = op->attr("q");
        if (!qa.valid()) { return {}; }
        const AttrValue qq = ctx.attr_value(qa);
        if (qq.kind != AttrKind::Float || !(qq.as_float() > 0.0)) { return {nullptr, op, AudioMisuseKind::BiquadQInvalid}; }
    }

    if (is_delay)
    {
        const AttrId dfa = op->attr("delay_frames"); // ABSENT -> generated verify_delay owns PRESENCE (check .valid() first)
        if (!dfa.valid()) { return {}; }
        const AttrValue df = ctx.attr_value(dfa);
        if (df.kind != AttrKind::Int || df.i < 0) { return {nullptr, op, AudioMisuseKind::DelayFramesInvalid}; }
    }

    if (is_compressor)
    {
        // ABSENT attr -> generated verify_compressor owns PRESENCE; check .valid() first so the KIND branch fires only on a
        // present-but-wrong-kind attr (absent reads as Int 0 -- the attr-reader-check-valid scar). threshold_db unrestricted.
        const AttrId ra = op->attr("ratio");
        if (!ra.valid()) { return {}; }
        const AttrValue rv = ctx.attr_value(ra);
        if (rv.kind != AttrKind::Float || !(rv.as_float() >= 1.0)) { return {nullptr, op, AudioMisuseKind::CompressorRatioInvalid}; }
        const AttrId aa = op->attr("attack_ms");
        if (!aa.valid()) { return {}; }
        const AttrValue av = ctx.attr_value(aa);
        if (av.kind != AttrKind::Float || !(av.as_float() > 0.0)) { return {nullptr, op, AudioMisuseKind::CompressorAttackInvalid}; }
        const AttrId rla = op->attr("release_ms");
        if (!rla.valid()) { return {}; }
        const AttrValue rlv = ctx.attr_value(rla);
        if (rlv.kind != AttrKind::Float || !(rlv.as_float() > 0.0)) { return {nullptr, op, AudioMisuseKind::CompressorReleaseInvalid}; }
    }
    return {};
}

AudioMisuse scan_audio_region(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const AudioMisuse e = check_op(ctx, op);
            if (e.kind != AudioMisuseKind::None) { return e; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const AudioMisuse ce = scan_audio_region(ctx, op->region(i));
                if (ce.kind != AudioMisuseKind::None) { return ce; }
            }
        }
    }
    return {};
}
} // namespace

AudioMisuse find_audio_misuse(const Context& ctx, const Module& m) { return scan_audio_region(ctx, m.body()); }

StringView audio_misuse_kind_name(AudioMisuseKind k) noexcept
{
    switch (k)
    {
    case AudioMisuseKind::None: return StringView("none");
    case AudioMisuseKind::OperandNotTensor: return StringView("operand-not-tensor");
    case AudioMisuseKind::ResultTypeMismatch: return StringView("result-type-mismatch");
    case AudioMisuseKind::BiquadFilterInvalid: return StringView("biquad-filter-invalid");
    case AudioMisuseKind::BiquadCutoffInvalid: return StringView("biquad-cutoff-invalid");
    case AudioMisuseKind::BiquadQInvalid: return StringView("biquad-q-invalid");
    case AudioMisuseKind::SourceStartFrameInvalid: return StringView("source-start-frame-invalid");
    case AudioMisuseKind::DelayFramesInvalid: return StringView("delay-frames-invalid");
    case AudioMisuseKind::CompressorRatioInvalid: return StringView("compressor-ratio-invalid");
    case AudioMisuseKind::CompressorAttackInvalid: return StringView("compressor-attack-invalid");
    case AudioMisuseKind::CompressorReleaseInvalid: return StringView("compressor-release-invalid");
    case AudioMisuseKind::SampleRateInvalid: return StringView("sample-rate-invalid");
    case AudioMisuseKind::TensorRankInvalid: return StringView("tensor-rank-invalid");
    case AudioMisuseKind::ChannelCountInvalid: return StringView("channel-count-invalid");
    case AudioMisuseKind::FrameDimInvalid: return StringView("frame-dim-invalid");
    }
    return StringView("?");
}
} // namespace crd::ceir::audio
