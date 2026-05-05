#version 460

layout(push_constant) uniform Push
{
    mat4 mvp;
} u_push;

layout(location = 0) in vec3 a_position;

void main()
{
    gl_Position = u_push.mvp * vec4(a_position, 1.0);
}
