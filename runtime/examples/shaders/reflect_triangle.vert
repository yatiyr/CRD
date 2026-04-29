#version 460

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 v_color;

layout(set = 0, binding = 0) uniform CameraData
{
    mat4 view_proj;
} camera_data;

layout(push_constant) uniform DrawData
{
    vec4 tint;
} draw_data;

void main()
{
    gl_Position = camera_data.view_proj * vec4(in_position, 0.0, 1.0);
    v_color = in_color * draw_data.tint.rgb;
}
