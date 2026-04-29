#version 460

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 1, binding = 0) uniform sampler2D albedo_tex;

void main()
{
    vec3 tex = texture(albedo_tex, vec2(0.5, 0.5)).rgb;
    out_color = vec4(v_color * tex, 1.0);
}
