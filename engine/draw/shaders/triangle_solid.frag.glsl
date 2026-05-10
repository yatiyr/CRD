// crd-draw -- triangle_solid.frag.glsl  (Phase 3.1 v1a-draw d1, ADR-0066 sec 5).
//
// Trivial pass-through: outputs the per-instance color with its alpha.
// Standard alpha blending in the pipeline composes over the existing scene.

#version 460

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = v_color;
}
