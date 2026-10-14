#version 440
// Waveform (p0.x = 0): tex is width x 1 RGBA32F with (min, max, avg) per column.
// Spectrogram (p0.x = 1): tex is width x height R32F intensities, row 0 = top.
// p0.y vertical zoom, p0.zw selection in u; c0 background, c1 selection,
// c2 waveform, c3 waveform average band.
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

vec3 palette(float v)
{
    // black -> blue -> magenta -> orange -> white
    vec3 a = mix(vec3(0.0), vec3(0.1, 0.1, 0.6), smoothstep(0.0, 0.25, v));
    a = mix(a, vec3(0.7, 0.1, 0.6), smoothstep(0.25, 0.5, v));
    a = mix(a, vec3(1.0, 0.6, 0.1), smoothstep(0.5, 0.75, v));
    return mix(a, vec3(1.0), smoothstep(0.75, 1.0, v));
}

void main()
{
    bool sel = uv.x >= p0.z && uv.x <= p0.w;
    vec4 bg = sel ? c1 : c0;
    if (p0.x < 0.5) {
        vec3 m = texture(tex, vec2(uv.x, 0.5)).rgb;
        float y = (0.5 - uv.y) * 2.0 / p0.y;
        if (y >= m.r && y <= m.g)
            fragColor = abs(y) <= m.b ? c3 : c2;
        else
            fragColor = bg;
    } else {
        float v = clamp(texture(tex, uv).r * p0.y, 0.0, 1.0);
        vec3 c = palette(v);
        fragColor = vec4(sel ? mix(c, c1.rgb, 0.25) : c, 1.0);
    }
}
