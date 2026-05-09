#pragma once

// crd-preset umbrella header (Phase 3.0 v1n; ADR-0059).
//
// The substrate ships in v1n1: registry + loader + artifact-builder + the
// IPresetTarget interface base. Concrete preset types arrive in subsequent
// sub-slices (QualityPreset in v1n2, CameraPreset in v1n3) with their own
// schema headers and IPresetTarget::apply() overloads — those headers are
// included alongside this one when their consumers need them.

#include <crd/preset/camera_preset.hpp>
#include <crd/preset/preset_apply_event.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/preset/preset_loader.hpp>
#include <crd/preset/preset_registry.hpp>
#include <crd/preset/preset_resolver.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/preset/preset_target.hpp>
#include <crd/preset/quality_preset.hpp>
