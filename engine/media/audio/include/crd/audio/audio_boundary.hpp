#pragma once

// audio_boundary.hpp — DIAG.10c: the sanity probes at the audio boundary.
//
// A wrong audio result rarely crashes. A buffer made at 44.1 kHz bound into a 48 kHz graph renders at the wrong pitch
// and length; a buffer that is not a whole number of frames shifts every channel after the first; a NaN from one source
// silences or poisons the whole bus; a callback that leaves flush-to-zero set changes every later period's arithmetic;
// a callback that writes stereo into a device buffer of another channel count garbles the layout. These probes catch
// each at the boundary where it arrives (docs/design/runtime-diagnostics.md, DIAG.10c):
// - check_source_binding: a source binding's shape (a whole number of frames, at least one) and time domain (its
//   declared sample rate against the graph's). A rate of 0 is undeclared: it still renders, as before, but is reported
//   as RateUndeclared, never as a match. Both renderers (render_graph, execute_audio_graph_ceir) refuse a binding that
//   does not render. A mismatch is a refusal, never a resample: the probe changes no algorithm and no unit.
// - audio_sample_domain: the value domain of a rendered sample. Finite; no bound, because headroom above 1 before the
//   master stage is legal; subnormals and -0 allowed.
// - render_audio_period: the render device's per-period work, separated from the device so it is testable without
//   audio hardware. The callback always runs in the declared numerical mode (crd/core/fp_environment.hpp) whatever mode
//   the device thread is in, and a mode it leaves set does not reach the next period; both are reported. What it wrote
//   is probed against audio_sample_domain. A device whose buffer has more than two channels gets the callback's stereo
//   in its first two channels through a scratch buffer the device allocates up front (nothing allocates per period).

#include <crd/audio/audio_graph.hpp>
#include <crd/audio/audio_realtime.hpp> // AudioRenderFn
#include <crd/core/types.hpp>
#include <crd/core/value_domain.hpp>

namespace crd::audio
{

// What check_source_binding found, worst last.
// NOLINTNEXTLINE(performance-enum-size)
enum class SourceBindingVerdict : crd::u8
{
    Bound,          // a whole number of frames at the graph's rate
    RateUndeclared, // a whole number of frames; the rate is undeclared (0), so the time domain is unchecked
    Unbound,        // no samples, or no channels
    PartialFrame,   // the sample count is not a whole number of frames (fewer than one frame included)
    RateMismatch,   // a declared rate other than the graph's
};
[[nodiscard]] const char* source_binding_verdict_name(SourceBindingVerdict v) noexcept;

// Judge one source binding against the rate (Hz) the graph renders at.
[[nodiscard]] SourceBindingVerdict check_source_binding(const GraphSourceBinding& binding,
                                                        crd::u32 graph_rate) noexcept;

// Whether a binding with this verdict renders.
[[nodiscard]] constexpr bool source_binding_renders(SourceBindingVerdict v) noexcept
{
    return v == SourceBindingVerdict::Bound || v == SourceBindingVerdict::RateUndeclared;
}

// The domain of a rendered sample (see the header comment).
[[nodiscard]] constexpr crd::FloatDomain audio_sample_domain() noexcept
{
    crd::FloatDomain d{};
    d.name = "audio-sample";
    return d;
}

// What one render period found.
struct AudioPeriodReport
{
    bool                   called              = false; // the callback ran (a callback, >= 2 device channels, scratch)
    crd::u32               entry_fp_difference = 0U;    // FpDifference bits: the device thread's mode vs the declared
    crd::u32               exit_fp_difference  = 0U;    // FpDifference bits: the mode the callback returned in vs it
    crd::ValueDomainReport samples{};                   // the frames x 2 stereo values the callback wrote
};

// One render period: silence `device_out` (frames x device_channels, interleaved), run `fn` in the declared mode into
// it (device_channels == 2) or into `stereo_scratch` (more channels; needs frames x 2 floats) and copy that stereo into
// the device's first two channels, put the thread's own mode back, and probe the stereo the callback wrote. With no
// callback, fewer than two device channels or too small a scratch, the period stays silent and the callback is not
// called. Allocates nothing.
[[nodiscard]] AudioPeriodReport render_audio_period(AudioRenderFn fn, void* user, crd::f32* device_out, crd::u32 frames,
                                                    crd::u32 device_channels, crd::u32 sample_rate,
                                                    crd::f32* stereo_scratch, crd::usize scratch_floats) noexcept;

} // namespace crd::audio
