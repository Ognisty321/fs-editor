#version 440
// Video frame or premultiplied overlay; p0.x = opacity.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    vec4 rect;   // target rectangle in NDC: x0, y0, x1, y1
    vec4 uvRect; // texture sub-rectangle: u0, v0, u1, v1
    vec4 p0;
    vec4 p1;
    vec4 c0;
    vec4 c1;
    vec4 c2;
    vec4 c3;
};
layout(binding = 1) uniform sampler2D tex;
void main()
{
    fragColor = texture(tex, uv) * p0.x;
}
