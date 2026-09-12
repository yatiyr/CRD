#pragma once

// crd-draw -- primitive renderer (Phase 3.1 v1a-draw, ADR-0066).
//
// Public umbrella header. Pulls in everything most consumers need:
//   - types.hpp: Color / PrimFlags / DepthMode / Category / Debug{Point,Line,Triangle,Text}
//   - render_buffer.hpp: RenderBuffer (the retained primitive store)
//   - shapes.hpp: shape generators (line / box_wire / aabb_wire); d1+ adds more
//
// d0 ships the data model + line + box wireframe. d1 adds solid pipeline +
// sphere/capsule. d2 adds full immediate-mode API. d3 adds DebugVizSystem +
// VisualizerRegistry + crd-eylem-viz. d4 adds ImGui control panel + sandbox
// demo.
//
// For the GPU rendering side (frame-graph overlay pass): see render_pass.hpp.

#include <crd/draw/active_buffer.hpp>
#include <crd/draw/debug_viz_component.hpp>
#include <crd/draw/debug_viz_system.hpp>
#include <crd/draw/default_visualizers.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/serialize.hpp>
#include <crd/draw/shapes.hpp>
#include <crd/draw/theme.hpp>
#include <crd/draw/types.hpp>
#include <crd/draw/visualizer_registry.hpp>
