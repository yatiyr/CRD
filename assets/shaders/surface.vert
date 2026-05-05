#version 460

layout(set = 0, binding = 0) uniform PerFrameUbo
{
    mat4 view;
    mat4 proj;
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 camera_pos_ws;
    float viewport_width;
    float viewport_height;
    float time_seconds;
    float _pad;
} u_frame;

layout(push_constant) uniform PerDrawPush
{
    mat4 model;
} u_push;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv0;
layout(location = 3) in vec4 a_tangent;

layout(location = 0) out vec3 v_position_ws;
layout(location = 1) out vec3 v_normal_ws;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec4 v_tangent_ws;

void main()
{
    vec4 pos_ws      = u_push.model * vec4(a_position, 1.0);
    v_position_ws    = pos_ws.xyz;
    v_normal_ws      = normalize(mat3(u_push.model) * a_normal);
    v_uv0            = a_uv0;
    v_tangent_ws     = vec4(normalize(mat3(u_push.model) * a_tangent.xyz), a_tangent.w);
    gl_Position      = u_frame.view_proj * pos_ws;
}
