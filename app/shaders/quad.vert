#version 440
// Unit quad mapped onto rect; shared by the video and audio views.
layout(location = 0) in vec2 pos;
layout(location = 0) out vec2 uv;
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
void main()
{
    uv = mix(uvRect.xy, uvRect.zw, pos);
    gl_Position = vec4(mix(rect.xy, rect.zw, pos), 0.0, 1.0);
}
