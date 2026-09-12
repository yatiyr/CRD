#pragma once

// audio_kernels.hpp — GEO-10 / CEIR-31a-1b: the per-node-per-block audio DSP STAGES, EXTRACTED from render_graph so the
// offline renderer AND the CEIR audio executor (ceir_audio_render, 31a-1b-ii) call the IDENTICAL code -- bit-exact BY
// CONSTRUCTION (the 30b-2b-1 identical-kernel precedent), not by mirroring. Each stage operates on a stereo f32 block bus
// `bus` of `len` frames (interleaved L,R). ⛔ params are f32 (matching the AGRF's stored f32 cutoff/q/gain_db) so a caller
// reading an f64 source (the CEIR attr) is FORCED by the signature to narrow through f32 -- the coefficient-divergence
// guard. Biquad state (f64 DF2T z1/z2 per channel) is passed IN, so a single-pass call [start=0,len=frames] and
// render_graph's per-block call are the SAME function with different slicing -- the single-pass==chunked bit-exactness
// argument made structural. ⛔ Gain AUTOMATION (per-sample dB) stays INLINE in render_graph (the automated branch computes
// a per-sample dB); the ceir.audio dialect has no automation node, so the CEIR side never takes that path.

#include <crd/audio/audio_graph.hpp> // GraphSourceBinding, BiquadType
#include <crd/core/types.hpp>
#include <crd/hesap/dsp/filter.hpp> // Biquad<T>
#include <crd/hesap/dsp/rbj.hpp>    // rbj_lowpass/highpass/bandpass/notch
#include <crd/math/cmath.hpp>       // pow

namespace crd::audio
{

// dB -> linear amplitude -- the ONE place the gain law is written (apply_gain + render_graph's automated branch call it).
[[nodiscard]] inline crd::f32 db_to_linear(crd::f32 db) { return crd::math::pow(10.0F, db / 20.0F); }

// Source: ADD a bound buffer's samples (from frame `start + start_frame`, optional loop; mono sources center) into `bus`.
inline void apply_source(crd::f32* bus, crd::i64 start, crd::i64 len, const GraphSourceBinding& b,
                         crd::i64 start_frame, bool loop)
{
    const crd::u64 bframes = b.samples.size() / b.channels;
    for (crd::i64 i = 0; i < len; ++i)
    {
        crd::u64 f = static_cast<crd::u64>(start + i) + static_cast<crd::u64>(start_frame);
        if (loop) { f %= bframes; }
        if (f >= bframes) { continue; } // one-shot past the end = silence
        if (b.channels >= 2)
        {
            bus[i * 2] += b.samples[f * b.channels];
            bus[i * 2 + 1] += b.samples[f * b.channels + 1];
        }
        else // mono centers
        {
            const crd::f32 s = b.samples[f];
            bus[i * 2] += s;
            bus[i * 2 + 1] += s;
        }
    }
}

// Gain/Send (constant dB): scale the stereo bus in place.
inline void apply_gain(crd::f32* bus, crd::i64 len, crd::f32 gain_db)
{
    const crd::f32 g = db_to_linear(gain_db);
    for (crd::i64 i = 0; i < len; ++i)
    {
        bus[i * 2] *= g;
        bus[i * 2 + 1] *= g;
    }
}

// Biquad (RBJ, DF2T): filter the stereo bus in place; f64 state z1/z2 per channel carried across calls. cutoff (Nyquist
// fraction) + q are f32, cast UP to f64 for the coefficient closed form (the AGRF-f32 narrowing the signature enforces).
inline void apply_biquad(crd::f32* bus, crd::i64 len, crd::u8 filter, crd::f32 cutoff, crd::f32 q, crd::f64 z1[2],
                         crd::f64 z2[2])
{
    crd::hesap::dsp::Biquad<crd::f64> bq;
    const crd::f64                    f0 = static_cast<crd::f64>(cutoff);
    const crd::f64                    qq = static_cast<crd::f64>(q);
    switch (static_cast<BiquadType>(filter))
    {
    case BiquadType::Lowpass: bq = crd::hesap::dsp::rbj_lowpass<crd::f64>(f0, qq); break;
    case BiquadType::Highpass: bq = crd::hesap::dsp::rbj_highpass<crd::f64>(f0, qq); break;
    case BiquadType::Bandpass: bq = crd::hesap::dsp::rbj_bandpass<crd::f64>(f0, qq); break;
    case BiquadType::Notch:
    default: bq = crd::hesap::dsp::rbj_notch<crd::f64>(f0, qq); break;
    }
    for (crd::i64 i = 0; i < len; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            const crd::f64 x = static_cast<crd::f64>(bus[i * 2 + c]);
            const crd::f64 y = bq.b0 * x + z1[c];
            z1[c]            = bq.b1 * x - bq.a1 * y + z2[c];
            z2[c]            = bq.b2 * x - bq.a2 * y;
            bus[i * 2 + c]   = static_cast<crd::f32>(y);
        }
    }
}

// Delay (integer sample shift): out[n] = in[n - delay_frames] for n >= delay_frames, +0.0F before. In-place, processed
// BACKWARD (n high->low) so in[n-D] (a LOWER index) is read before it is overwritten. ⛔ `n < delay_frames` is a SIGNED
// compare -- do not hoist n-D into unsigned. D == 0 is the identity (self-assign); D >= len yields an all-zero buffer.
// Single-pass over the whole buffer (no history state) -- the CEIR executor's model; render_graph has no delay node.
inline void apply_delay(crd::f32* bus, crd::i64 len, crd::i64 delay_frames)
{
    for (crd::i64 n = len - 1; n >= 0; --n)
    {
        for (int c = 0; c < 2; ++c)
        {
            bus[n * 2 + c] = (n >= delay_frames) ? bus[(n - delay_frames) * 2 + c] : 0.0F;
        }
    }
}

// Compressor (feed-forward peak, stereo-LINKED): reduce the stereo bus's gain in place per the static curve. The detector
// is the linked peak d = max(|L|,|R|); a linear-domain one-pole envelope tracks it (env = c*env + (1-c)*d, c = attack when
// d>env else release, c = exp(-1/(ms*1e-3*sr))); the hard-knee curve on 20*log10(env) yields a gain applied to BOTH
// channels. ⛔ UPDATE-then-gain: env is advanced for THIS sample before its gain is computed (feed-forward; the reference
// gate uses the same convention -- a swap is a one-sample skew that reads as a libm miss). ⛔ apply the f64 gain in f64,
// narrow ONCE (`(f32)(x*gain)`, not `x*(f32)gain` -- a second rounding of gain). params f32 (attrs narrowed through f32, the
// signature discipline), envelope math f64. `env` is the ONE linked envelope state (a scalar, threaded across blocks). NO
// makeup gain (a following audio.gain). The 1e-9 log floor is a DESIGN floor (env=0 -> below threshold -> gain=1; the value
// never reaches the output), not a numeric fudge.
inline void apply_compressor(crd::f32* bus, crd::i64 len, crd::f32 threshold_db, crd::f32 ratio, crd::f32 attack_ms,
                             crd::f32 release_ms, crd::u32 sample_rate, crd::f64& env)
{
    const crd::f64 sr        = static_cast<crd::f64>(sample_rate);
    const crd::f64 atk_coeff = crd::math::exp(-1.0 / (static_cast<crd::f64>(attack_ms) * 1.0e-3 * sr));
    const crd::f64 rel_coeff = crd::math::exp(-1.0 / (static_cast<crd::f64>(release_ms) * 1.0e-3 * sr));
    const crd::f64 thr_db    = static_cast<crd::f64>(threshold_db);
    const crd::f64 slope     = 1.0 / static_cast<crd::f64>(ratio) - 1.0; // <= 0 for ratio >= 1 (gain reduction)
    for (crd::i64 n = 0; n < len; ++n)
    {
        const crd::f64 l     = static_cast<crd::f64>(bus[n * 2]);
        const crd::f64 r     = static_cast<crd::f64>(bus[n * 2 + 1]);
        const crd::f64 d     = crd::math::max(crd::math::abs(l), crd::math::abs(r)); // peak, stereo-LINKED
        const crd::f64 coeff = (d > env) ? atk_coeff : rel_coeff;                    // attack rising, release falling
        env                  = coeff * env + (1.0 - coeff) * d;                      // UPDATE this sample, then gain
        const crd::f64 env_db  = 20.0 * crd::math::log10(crd::math::max(env, 1.0e-9));
        const crd::f64 over    = env_db - thr_db;
        const crd::f64 gain_db = (over > 0.0) ? over * slope : 0.0; // hard knee: <= 0 above threshold, unity (0) below
        const crd::f64 gain    = crd::math::pow(10.0, gain_db / 20.0);
        bus[n * 2]             = static_cast<crd::f32>(l * gain); // narrow ONCE (gain applied in f64)
        bus[n * 2 + 1]         = static_cast<crd::f32>(r * gain);
    }
}

} // namespace crd::audio
