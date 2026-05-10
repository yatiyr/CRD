// crd-draw -- infinite_grid.frag.glsl  (Phase 3.1 v1a-draw d2-grid, ADR-0066 sec 19).
//
// Procedural multi-scale grid with distance-from-camera fade. Inspired by
// the "Best Darn Grid Shader (Yet)" technique used across Blender, Unity
// editor, Houdini, etc.
//
// Emits two grid scales:
//   - Primary (every primary_cell metres): brighter
//   - Secondary (every secondary_cell metres): dimmer subdivisions
// Plus a hard-edge highlight on the X / Z world axes (red / blue) to
// orient the viewer.
// Alpha fades from 1.0 (at camera) -> 0.0 (at fade_distance) using
// quadratic falloff so the horizon is gentle.

#version 460

layout(push_constant) uniform PushConstants
{
    mat4  view_proj;
    vec2  viewport_px;
    uint  category_mask;
    float time_s;
    vec4  camera_pos;
    uint  primary_color;
    uint  secondary_color;
    float plane_y;
    float primary_cell;
    float secondary_cell;
    float fade_distance;
    uint  axis_x_color;     // RGBA8 packed (line at z=0)
    uint  axis_z_color;     // RGBA8 packed (line at x=0)
} u_push;

layout(location = 0) in  vec3 v_world_pos;
layout(location = 0) out vec4 out_color;

vec4 unpack_rgba8(uint p)
{
    return vec4(
        float( p        & 0xFFu),
        float((p >>  8) & 0xFFu),
        float((p >> 16) & 0xFFu),
        float((p >> 24) & 0xFFu)
    ) * (1.0 / 255.0);
}

// Anti-aliased grid line intensity at world-space coordinate `coord` with
// cell pitch `cell`. Output ~1.0 on a line, ~0.0 between lines, smoothed
// by 1-pixel-equivalent screen-space derivatives. Standard pristineGrid
// trick from Inigo Quilez / Best Darn Grid.
float grid_factor(vec2 coord, float cell)
{
    vec2 g = abs(fract(coord / cell - 0.5) - 0.5) / fwidth(coord / cell);
    float line = min(g.x, g.y);
    return 1.0 - clamp(line, 0.0, 1.0);
}

void main()
{
    vec2 grid_uv = v_world_pos.xz;

    float primary   = grid_factor(grid_uv, u_push.primary_cell);
    float secondary = grid_factor(grid_uv, u_push.secondary_cell);

    // Distance-from-camera quadratic fade (in XZ plane).
    vec2  delta_xz = v_world_pos.xz - u_push.camera_pos.xz;
    float dist_xz  = length(delta_xz);
    float fade     = 1.0 - clamp(dist_xz / max(u_push.fade_distance, 1.0), 0.0, 1.0);
    fade = fade * fade;

    vec4 col_primary   = unpack_rgba8(u_push.primary_color);
    vec4 col_secondary = unpack_rgba8(u_push.secondary_color);

    // Compose: secondary first (dimmer), then primary on top.
    vec4 result = col_secondary * secondary;
    result      = mix(result, col_primary, primary);

    // Axis highlights -- themable colors (default Blender / RViz convention:
    // X axis = red, Z axis = blue). The line at z=0 is the X axis (varying
    // X with Z held at zero); the line at x=0 is the Z axis.
    vec4 col_axis_x = unpack_rgba8(u_push.axis_x_color);
    vec4 col_axis_z = unpack_rgba8(u_push.axis_z_color);
    float axis_x_factor = 1.0 - clamp(abs(v_world_pos.x) / fwidth(v_world_pos.x), 0.0, 1.0);
    float axis_z_factor = 1.0 - clamp(abs(v_world_pos.z) / fwidth(v_world_pos.z), 0.0, 1.0);
    if (axis_z_factor > 0.0) result = mix(result, col_axis_x, axis_z_factor); // line at z=0 = X axis
    if (axis_x_factor > 0.0) result = mix(result, col_axis_z, axis_x_factor); // line at x=0 = Z axis

    out_color = vec4(result.rgb, result.a * fade);
}
