// crd-draw -- infinite_grid.vert.glsl  (Phase 3.1 v1a-draw d2-grid, ADR-0066 sec 19).
//
// Emits a fullscreen-covering quad on the y = `plane_y` plane in world
// space. The quad is sized to extend `fade_distance * 2` in each direction
// from the camera, so distance-fade math in the fragment never sees
// out-of-bounds geometry. Single draw call, 4 vertices, no vertex buffer.
//
// Push constants (shared layout with draw pipelines, 128 bytes total).

#version 460

layout(push_constant) uniform PushConstants
{
    mat4  view_proj;
    vec2  viewport_px;
    uint  category_mask;
    float time_s;
    // Grid-specific (next 64 bytes -- ignored by line/triangle shaders).
    vec4  camera_pos;       // xyz = camera world position; w = unused
    uint  primary_color;    // RGBA8 packed
    uint  secondary_color;  // RGBA8 packed
    float plane_y;          // world Y of the grid plane
    float primary_cell;     // primary grid cell size in metres
    float secondary_cell;   // secondary (subdivision) cell size in metres
    float fade_distance;    // distance from camera at which alpha = 0
    uint  axis_x_color;     // RGBA8 packed (line at z=0)
    uint  axis_z_color;     // RGBA8 packed (line at x=0)
} u_push;

// 6 vertices = 2 triangles forming a unit XZ quad. RHI exposes only
// TriangleList topology, so we expand here instead of relying on
// TriangleStrip.
layout(location = 0) out vec3 v_world_pos;

void main()
{
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0,  1.0)
    );
    vec2 corner = corners[gl_VertexIndex % 6];

    // Anchor the quad on the camera's XZ position so it always covers the
    // visible region. Half-extent = fade_distance so distance fade fits.
    float half_extent = max(u_push.fade_distance, 1.0);
    vec3  world = vec3(u_push.camera_pos.x + corner.x * half_extent,
                       u_push.plane_y,
                       u_push.camera_pos.z + corner.y * half_extent);

    v_world_pos  = world;
    gl_Position  = u_push.view_proj * vec4(world, 1.0);
}
