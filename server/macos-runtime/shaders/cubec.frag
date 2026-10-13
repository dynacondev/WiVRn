#version 450
layout(location = 0) in vec3 o;
layout(location = 0) out vec4 f;
void main()
{
    f = vec4(o, 1.0);
}
