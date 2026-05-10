// crd-draw — line_aa.vert.glsl  (Phase 3.1 v1a-draw-d0, ADR-0066 §4)
//
// Screen-space quad expansion for anti-aliased thick lines (Three.js
// LineSegments2 / Mapbox technique). Each line is one INSTANCE that draws
// 6 vertices (two triangles forming a quad). gl_VertexIndex selects which
// of the 6 corners we are.
//
// Per-instance attributes (binding 0, vertex-input rate Instance):
//   loc 0  vec3  start     (line endpoint A, world space)
//   loc 1  vec3  end       (line endpoint B, world space)
//   loc 2  uint  color_packed  (RGBA8 packed; matches crd::draw::Color::packed_rgba())
//   loc 3  uint  flags     (PrimFlags raw u32)
//   loc 4  float width     (pixels by default; world units if flags bit 6 set)
//
// Per-frame uniforms (push-constants, 80 bytes — fits Vulkan 128-byte minimum):
//   mat4  view_proj  (camera VP matrix)
//   vec2  viewport_px  (screen dimensions in pixels)
//   uint  category_mask (filter; instance discarded if (1u << flags.category) not set)
//   float time_s    (for lifetime fade)
//
// Outputs (varyings to fragment):
//   loc 0  vec4  v_color
//   loc 1  vec2  v_quad_coord  (-1..+1 across the line's width; 0..1 along its length)
//   loc 2  flat float v_width_px  (line thickness in pixels, for AA falloff)

#version 460

layout(push_constant) uniform PushConstants
{
    mat4  view_proj;
    vec2  viewport_px;
    uint  category_mask;
    float time_s;
} u_push;

layout(location = 0) in vec3 in_start;
layout(location = 1) in vec3 in_end;
layout(location = 2) in uint in_color_packed;
layout(location = 3) in uint in_flags;
layout(location = 4) in float in_width;

layout(location = 0) out vec4  v_color;
layout(location = 1) out vec2  v_quad_coord;
layout(location = 2) flat out float v_width_px;

// 6-corner unit quad (two triangles: 0,1,2 + 2,1,3 in non-strip order).
// Layout: x = along-line in [0, 1] (start..end); y = perpendicular in [-1, +1].
//
//    2 +----+ 3
//      |\\  |
//      |  \\|
//    0 +----+ 1
const vec2 kCorners[6] = vec2[6](
    vec2(0.0, -1.0),  // 0  start, -side
    vec2(1.0, -1.0),  // 1  end,   -side
    vec2(0.0,  1.0),  // 2  start, +side
    vec2(0.0,  1.0),  // 3  start, +side  (second triangle)
    vec2(1.0, -1.0),  // 4  end,   -side
    vec2(1.0,  1.0)   // 5  end,   +side
);

vec4 unpack_rgba8(uint p)
{
    return vec4(
        float( p        & 0xFFu),
        float((p >>  8) & 0xFFu),
        float((p >> 16) & 0xFFu),
        float((p >> 24) & 0xFFu)
    ) * (1.0 / 255.0);
}

void main()
{
    // Category-mask filter: collapse the instance to a degenerate point
    // off-screen if its category bit isn't set. Cheaper than a discard
    // in the fragment shader (zero rasterisation work).
    uint cat = (in_flags >> 2) & 0xFu;
    if ((u_push.category_mask & (1u << cat)) == 0u)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside NDC
        v_color     = vec4(0.0);
        v_quad_coord = vec2(0.0);
        v_width_px   = 0.0;
        return;
    }

    vec2 corner = kCorners[gl_VertexIndex % 6];

    // Transform both endpoints to clip space.
    vec4 clip_a = u_push.view_proj * vec4(in_start, 1.0);
    vec4 clip_b = u_push.view_proj * vec4(in_end,   1.0);

    // Project to NDC (homogeneous divide).
    vec2 ndc_a = clip_a.xy / clip_a.w;
    vec2 ndc_b = clip_b.xy / clip_b.w;

    // Screen-space (pixel) positions of the endpoints.
    vec2 px_a = ndc_a * 0.5 * u_push.viewport_px;
    vec2 px_b = ndc_b * 0.5 * u_push.viewport_px;

    // Screen-space line direction + perpendicular.
    vec2 dir_px = px_b - px_a;
    float len_px = length(dir_px);
    if (len_px < 1e-3) { dir_px = vec2(1.0, 0.0); len_px = 1.0; }
    vec2 dir = dir_px / len_px;
    vec2 perp = vec2(-dir.y, dir.x);

    // Width in pixels: use in_width as-is (treating it as pixels for v0 of
    // d0; flags bit 6 = world_units interpretation reserved for d2+ refinement).
    float half_w_px = in_width * 0.5;

    // Push the corner out perpendicular by width/2, lerp along the line by corner.x.
    vec2 base_px = mix(px_a, px_b, corner.x);
    vec2 push_px = perp * (corner.y * half_w_px);
    vec2 final_px = base_px + push_px;

    // Back to NDC.
    vec2 final_ndc = final_px * 2.0 / u_push.viewport_px;

    // Pick the right w-component (start vs end) for proper perspective.
    float w = mix(clip_a.w, clip_b.w, corner.x);
    float z = mix(clip_a.z / clip_a.w, clip_b.z / clip_b.w, corner.x);

    gl_Position  = vec4(final_ndc * w, z * w, w);
    v_color      = unpack_rgba8(in_color_packed);
    v_quad_coord = corner;
    v_width_px   = in_width;
}
