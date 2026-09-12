#include <crd/renderasset/binding.hpp>

namespace crd::renderasset
{
StringView binding_frequency_name(BindingFrequency frequency) noexcept
{
    switch (frequency)
    {
    case BindingFrequency::Frame:
        return "frame";
    case BindingFrequency::Pass:
        return "pass";
    case BindingFrequency::Material:
        return "material";
    case BindingFrequency::Object:
        return "object";
    case BindingFrequency::Draw:
        return "draw";
    }
    return "unknown";
}

StringView binding_kind_name(BindingKind kind) noexcept
{
    switch (kind)
    {
    case BindingKind::StorageBuffer:
        return "storage-buffer";
    case BindingKind::UniformBuffer:
        return "uniform-buffer";
    case BindingKind::SampledTexture:
        return "sampled-texture";
    case BindingKind::Sampler:
        return "sampler";
    case BindingKind::ComparisonSampler:
        return "comparison-sampler";
    case BindingKind::BindlessTextureArray:
        return "bindless-texture-array";
    }
    return "unknown";
}
} // namespace crd::renderasset
