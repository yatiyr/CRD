#pragma once

// crd-draw -- default visualizers (Phase 3.1 v1a-draw d3, ADR-0066 sec 11-12).
//
// `register_default_visualizers(registry)` populates the registry with
// crd-draw's built-in visualizers. Today: just one --
//
//   `crd::scene::Transform`  →  axis triad at the entity's world transform
//                               (length scaled by DebugVizComponent::scale,
//                                emitted only when the AxisTriad flag is set)
//
// Module-specific visualizers (eylem-viz, audio-viz, ...) live in their
// own modules and call their own register_*_visualizers(registry) helper.
// Cerid's policy is one-call-per-module so misregistration is loud.

namespace crd::draw
{
class VisualizerRegistry;

// Register crd-draw's built-in visualizers. Idempotency: calling this
// twice will register Transform twice; visualizers will fire twice per
// entity. Call exactly once, alongside any module-specific helpers.
void register_default_visualizers(VisualizerRegistry& registry);

} // namespace crd::draw
