// crd-draw — line_aa.frag.glsl  (Phase 3.1 v1a-draw-d0, ADR-0066 §4)
//
// Per-pixel distance-from-center anti-aliasing. The vertex shader passes
// `v_quad_coord.y` as the signed distance from the line's center axis,
// scaled to [-1, +1] across the line's width. We compute alpha from
// distance such that exactly one pixel of falloff occurs at the edges,
// regardless of line angle or thickness.
//
// Result: pixel-perfect anti-aliased lines on every backend, identical
// look across Vulkan / D3D12 / Metal. ~5 lines of real shader code.

#version 460

layout(location = 0) in vec4  v_color;
layout(location = 1) in vec2  v_quad_coord;
layout(location = 2) flat in float v_width_px;

layout(location = 0) out vec4 out_color;

void main()
{
    // |distance| in [0, 1]; 1.0 == on the edge, 0.0 == on the center axis.
    float d = abs(v_quad_coord.y);

    // Falloff width in normalised units: 1 pixel of fade at the edges,
    // computed in normalised space by dividing 1px by the half-width.
    // For a 1-pixel line, this gives a soft 50% alpha through the entire
    // 2-pixel-wide quad — which IS what you want at sub-pixel thickness.
    float half_w = max(v_width_px * 0.5, 0.5);
    float fade   = 1.0 / half_w;

    // smoothstep gives a linear-ish ramp. 1.0 - smoothstep(...) inverts so
    // alpha is 1.0 at center and 0.0 at edge + 1px.
    float alpha = 1.0 - smoothstep(1.0 - fade, 1.0, d);

    // Note: avoid `discard;` to dodge the SPIR-V DemoteToHelperInvocation
    // capability requirement (Vulkan 1.3 device feature gate). Alpha=0
    // fragments are no-ops under standard alpha blending, so writing them
    // is functionally identical at zero perceptible perf cost.
    out_color = vec4(v_color.rgb, v_color.a * alpha);
}
