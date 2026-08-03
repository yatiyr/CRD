#pragma once

// crd-render-asset-core — the shared BINDING vocabulary (RAF-4, mission §9).
//
// A resource's binding FREQUENCY (how often it rebinds) and KIND (what it is) are ONE concept used by both the shader/
// program CONTRACT (crd-render-program) and the GPU COMMAND model (gpu-context's command_model.hpp). Defined ONCE here
// — the canonical identity/contract module — so there is a single definition, never one enum per layer.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::renderasset
{
using crd::containers::StringView;

// How often a binding changes — the rebind-frequency tiers (rarest → most frequent). A cooked binding layout groups
// resources by frequency so a renderer rebinds Frame/Pass data once and Object/Draw data per item.
enum class BindingFrequency : u8
{
    Frame = 0, // per-frame (camera, time)
    Pass,      // per-pass (targets, pass constants)
    Material,  // per-material (textures, params)
    Object,    // per-object (transforms)
    Draw,      // per-draw (push data)
};

// What a binding is. Closed set (the cooker verifies what an asset asked for — an open set could only be obeyed).
enum class BindingKind : u8
{
    StorageBuffer = 0,    // a pull/structured buffer
    UniformBuffer,        // a constant buffer
    SampledTexture,       // a sampled image
    Sampler,              // a filtering sampler
    ComparisonSampler,    // a shadow-compare sampler
    BindlessTextureArray, // a dynamic-indexed texture array
};

[[nodiscard]] StringView binding_frequency_name(BindingFrequency frequency) noexcept;
[[nodiscard]] StringView binding_kind_name(BindingKind kind) noexcept;
} // namespace crd::renderasset
