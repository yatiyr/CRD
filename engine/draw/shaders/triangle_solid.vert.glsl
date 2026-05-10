// crd-draw -- triangle_solid.vert.glsl  (Phase 3.1 v1a-draw d1, ADR-0066 sec 5).
//
// Solid translucent triangle rendering. Each triangle is one INSTANCE that
// emits 3 vertices. Per-instance attributes carry the 3 corners + color +
// flags; gl_VertexIndex (0..2) selects which corner this invocation is.
//
// Per-instance attributes (binding 0, vertex-input rate Instance):
//   loc 0  vec3  v0          (corner 0, world space)
//   loc 1  vec3  v1          (corner 1, world space)
//   loc 2  vec3  v2          (corner 2, world space)
//   loc 3  uint  color_packed (RGBA8 packed)
//   loc 4  uint  flags       (PrimFlags raw u32)
//
// Push-constants (shared with line_aa.vert.glsl, same 80-byte layout).

#version 460

layout(push_constant) uniform PushConstants
{
    mat4  view_proj;
    vec2  viewport_px;
    uint  category_mask;
    float time_s;
} u_push;

layout(location = 0) in vec3 in_v0;
layout(location = 1) in vec3 in_v1;
layout(location = 2) in vec3 in_v2;
layout(location = 3) in uint in_color_packed;
layout(location = 4) in uint in_flags;

layout(location = 0) out vec4 v_color;

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
    // Category-mask filter: collapse off-screen if disabled.
    uint cat = (in_flags >> 2) & 0xFu;
    if ((u_push.category_mask & (1u << cat)) == 0u)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_color     = vec4(0.0);
        return;
    }

    // Pick the corner this vertex represents (0, 1, or 2).
    vec3 world_pos;
    if      (gl_VertexIndex == 0) world_pos = in_v0;
    else if (gl_VertexIndex == 1) world_pos = in_v1;
    else                          world_pos = in_v2;

    gl_Position = u_push.view_proj * vec4(world_pos, 1.0);
    v_color     = unpack_rgba8(in_color_packed);
}
