// audio_boundary.cpp — DIAG.10c: the audio boundary's shape, time-domain, value-domain and mode probes (see header).

#include <crd/audio/audio_boundary.hpp>

#include <crd/core/fp_environment.hpp>

namespace crd::audio
{

const char* source_binding_verdict_name(SourceBindingVerdict v) noexcept
{
    switch (v)
    {
    case SourceBindingVerdict::Bound:
        return "bound";
    case SourceBindingVerdict::RateUndeclared:
        return "rate-undeclared";
    case SourceBindingVerdict::Unbound:
        return "unbound";
    case SourceBindingVerdict::PartialFrame:
        return "partial-frame";
    case SourceBindingVerdict::RateMismatch:
        return "rate-mismatch";
    }
    return "unknown";
}

SourceBindingVerdict check_source_binding(const GraphSourceBinding& binding, crd::u32 graph_rate) noexcept
{
    if (binding.samples.size() == 0U || binding.channels == 0U)
    {
        return SourceBindingVerdict::Unbound;
    }
    if (binding.samples.size() % binding.channels != 0U)
    {
        return SourceBindingVerdict::PartialFrame; // also fewer samples than one frame
    }
    if (binding.sample_rate == 0U)
    {
        return SourceBindingVerdict::RateUndeclared;
    }
    if (binding.sample_rate != graph_rate)
    {
        return SourceBindingVerdict::RateMismatch;
    }
    return SourceBindingVerdict::Bound;
}

AudioPeriodReport render_audio_period(AudioRenderFn fn, void* user, crd::f32* device_out, crd::u32 frames,
                                      crd::u32 device_channels, crd::u32 sample_rate, crd::f32* stereo_scratch,
                                      crd::usize scratch_floats) noexcept
{
    AudioPeriodReport report{};
    const crd::usize device_floats = static_cast<crd::usize>(frames) * device_channels;
    for (crd::usize i = 0U; i < device_floats; ++i)
    {
        device_out[i] = 0.0F;
    }
    const crd::usize stereo_floats = static_cast<crd::usize>(frames) * 2U;
    if (fn == nullptr || device_channels < 2U)
    {
        return report;
    }
    crd::f32* target = device_out;
    if (device_channels != 2U)
    {
        if (stereo_scratch == nullptr || scratch_floats < stereo_floats)
        {
            return report;
        }
        target = stereo_scratch;
        for (crd::usize i = 0U; i < stereo_floats; ++i)
        {
            target[i] = 0.0F;
        }
    }

    const crd::FpEnvironment declared = crd::declared_fp_environment();
    report.entry_fp_difference        = crd::fp_differences(crd::read_fp_environment(), declared);
    {
        const crd::ScopedFpEnvironment scope(declared); // puts the device thread's own mode back afterwards
        fn(user, target, frames, sample_rate);
        report.exit_fp_difference = crd::fp_differences(crd::read_fp_environment(), declared);
    }
    report.called  = true;
    report.samples = crd::probe_values(audio_sample_domain(), target, stereo_floats);

    if (target != device_out)
    {
        for (crd::usize f = 0U; f < frames; ++f)
        {
            device_out[f * device_channels]      = target[f * 2U];
            device_out[f * device_channels + 1U] = target[f * 2U + 1U];
        }
    }
    return report;
}

} // namespace crd::audio
