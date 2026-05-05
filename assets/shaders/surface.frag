#version 460

#include "../../engine/renderer/include/crd/renderer/surface_data.glsl.inc"

layout(location = 0) in vec3 v_position_ws;
layout(location = 1) in vec3 v_normal_ws;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec4 v_tangent_ws;

layout(location = 0) out vec4 out_color;

void crd_evaluate_surface(in VertexAttrs attrs, inout SurfaceData sd)
{
    sd.base_color = vec3(1.0);
    sd.normal_ws  = attrs.normal_ws;
    sd.metallic   = 0.0;
    sd.roughness  = 1.0;
    sd.emissive   = vec3(0.0);
    sd.occlusion  = 1.0;
}

void main()
{
    VertexAttrs attrs;
    attrs.position_ws = v_position_ws;
    attrs.normal_ws   = normalize(v_normal_ws);
    attrs.uv0         = v_uv0;
    attrs.tangent_ws  = v_tangent_ws;

    SurfaceData sd;
    sd.base_color = vec3(1.0);
    sd.normal_ws  = attrs.normal_ws;
    sd.metallic   = 0.0;
    sd.roughness  = 1.0;
    sd.emissive   = vec3(0.0);
    sd.occlusion  = 1.0;

    crd_evaluate_surface(attrs, sd);

    const vec3 light_dir = normalize(vec3(0.5, 1.0, 0.5));
    float n_dot_l        = max(dot(sd.normal_ws, light_dir), 0.0);
    vec3 color           = vec3(0.05) * sd.base_color + sd.base_color * n_dot_l + sd.emissive;

    out_color = vec4(color, 1.0);
}
