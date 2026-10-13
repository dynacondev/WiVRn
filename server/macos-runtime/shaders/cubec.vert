#version 450
layout(push_constant) uniform PC {
    mat4 mvp;
} pc;
layout(location = 0) in vec3 p;
layout(location = 1) in vec3 c;
layout(location = 0) out vec3 o;
void main()
{
    o = c;
    gl_Position = pc.mvp * vec4(p, 1.0);
}
